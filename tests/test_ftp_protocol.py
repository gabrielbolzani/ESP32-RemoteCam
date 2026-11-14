"""Testa o protocolo C real por FTP em loopback; não acessa a placa."""
from pathlib import Path
import ftplib
import io
import os
import re
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FtpProtocolTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = Path(os.environ.get("TCC_EXE", ROOT / "verification/tools/tcc/tcc.exe"))
        headers = Path(os.environ.get("TCC_WINAPI", ROOT / "verification/tools/winapi-full-for-0.9.27/include"))
        if not compiler.is_file() or not (headers / "winapi/winsock2.h").is_file():
            raise unittest.SkipTest("Defina TCC_EXE e TCC_WINAPI para o teste C em loopback.")
        cls.temporary = tempfile.TemporaryDirectory(prefix="ftp-check-", dir=ROOT / "verification")
        cls.folder = Path(cls.temporary.name)
        cls.card = cls.folder / "sd"
        cls.card.mkdir()
        (cls.card / "sub pasta").mkdir()
        (cls.card / "active").mkdir()
        cls.binary = bytes(range(256)) * 17
        (cls.card / "foto.bin").write_bytes(cls.binary)
        (cls.card / "texto.txt").write_bytes(b"linha1\nlinha2\r\nultima")
        (cls.card / "sub pasta/foto-ç.jpg").write_bytes(b"JPEG-test\x00\xff")
        (cls.card / "active/frame.jpg").write_bytes(b"complete")
        (cls.card / "active/frame.part").write_bytes(b"partial")
        (cls.card / "active/manifest.json").write_bytes(b"{}")
        (cls.card / "drain.bin").write_bytes(bytes(range(256)) * 2048)
        (cls.card / "large.bin").write_bytes(b"x" * (4 * 1024 * 1024))
        executable = cls.folder / "ftp-fixture.exe"
        result = subprocess.run([
            str(compiler), "-I" + str(headers / "winapi"), "-I" + str(headers),
            "-o", str(executable), str(ROOT / "tests/ftp_fixture.c"), "-lws2_32"
        ], capture_output=True, text=True)
        if result.returncode:
            cls.temporary.cleanup()
            raise RuntimeError(result.stderr)
        cls.process = subprocess.Popen(
            [str(executable), str(cls.card)], stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0)
        )
        line = cls.process.stdout.readline().strip()
        if not line.isdigit():
            cls.process.kill()
            raise RuntimeError("Servidor de teste não iniciou: " + line)
        cls.port = int(line)

    @classmethod
    def tearDownClass(cls):
        cls.process.terminate()
        cls.process.wait(timeout=5)
        cls.process.stdout.close()
        cls.process.stderr.close()
        cls.temporary.cleanup()

    def connect(self):
        client = ftplib.FTP(timeout=8, encoding="utf-8")
        client.connect("127.0.0.1", self.port)
        client.login()
        return client

    def setUp(self):
        self.ftp = self.connect()

    def tearDown(self):
        try:
            self.ftp.quit()
        except (OSError, ftplib.Error):
            self.ftp.close()
        for name in ("busy.flag", "recording.flag", "ftp-disabled.flag"):
            (self.card / name).unlink(missing_ok=True)
        time.sleep(0.04)

    def test_anonymous_browse_utf8_and_binary(self):
        self.assertEqual(self.ftp.pwd(), "/")
        entries = dict(self.ftp.mlsd())
        self.assertEqual(entries["sub pasta"]["type"], "dir")
        self.ftp.cwd("sub pasta")
        self.assertEqual(self.ftp.pwd(), "/sub pasta")
        result = bytearray()
        self.ftp.retrbinary("RETR foto-ç.jpg", result.extend)
        self.assertEqual(result, b"JPEG-test\x00\xff")
        self.ftp.cwd("..")
        result.clear()
        self.ftp.retrbinary("RETR foto.bin", result.extend)
        self.assertEqual(result, self.binary)

    def test_epsv_and_resume(self):
        self.ftp.voidcmd("TYPE I")
        reply = self.ftp.sendcmd("EPSV")
        port = int(re.search(r"\(\|\|\|(\d+)\|\)", reply).group(1))
        with socket.create_connection(("127.0.0.1", port), timeout=5) as data:
            self.ftp.sendcmd("REST 257")
            self.assertTrue(self.ftp.sendcmd("RETR foto.bin").startswith("150"))
            content = bytearray()
            while True:
                chunk = data.recv(8192)
                if not chunk:
                    break
                content.extend(chunk)
        self.assertTrue(self.ftp.voidresp().startswith("226"))
        self.assertEqual(content, self.binary[257:])
        other = bytearray()
        self.ftp.retrbinary("RETR foto.bin", other.extend, rest=13)
        self.assertEqual(other, self.binary[13:])

    def test_ascii_transfer_and_metadata(self):
        lines = []
        self.ftp.retrlines("RETR texto.txt", lines.append)
        self.assertEqual(lines, ["linha1", "linha2", "ultima"])
        self.ftp.voidcmd("TYPE I")
        self.assertEqual(self.ftp.size("foto.bin"), len(self.binary))
        self.assertRegex(self.ftp.sendcmd("MDTM foto.bin"), r"^213 [0-9]{14}$")
        self.assertIn("type=file;", self.ftp.sendcmd("MLST foto.bin"))
        listing = []
        self.ftp.retrlines("LIST", listing.append)
        self.assertTrue(any("foto.bin" in row and row.startswith("-r--r--r--") for row in listing))

    def test_write_commands_and_active_mode_are_denied(self):
        original = (self.card / "foto.bin").read_bytes()
        for command in ["STOR injected.bin", "APPE foto.bin", "DELE foto.bin", "RNFR foto.bin", "RNTO changed.bin", "MKD injected", "RMD active", "SITE CHMOD 777 foto.bin", "PORT 127,0,0,1,0,22", "EPRT |1|127.0.0.1|22|"]:
            with self.subTest(command=command):
                with self.assertRaises(ftplib.error_perm):
                    self.ftp.sendcmd(command)
        self.assertEqual((self.card / "foto.bin").read_bytes(), original)
        self.assertFalse((self.card / "injected.bin").exists())
        self.assertFalse((self.card / "injected").exists())

    def test_cannot_escape_sd_root(self):
        for target in ["../../fora", "/../../fora", "C:\\Users", "/sub pasta/../../../fora"]:
            with self.subTest(target=target):
                with self.assertRaises(ftplib.error_perm):
                    self.ftp.cwd(target)
        self.assertEqual(self.ftp.pwd(), "/")
        self.ftp.sendcmd("CDUP")
        self.assertEqual(self.ftp.pwd(), "/")

    def test_busy_and_recording_policy_propagates_without_leak(self):
        (self.card / "busy.flag").write_text("busy")
        with self.assertRaises(ftplib.error_temp):
            self.ftp.nlst()
        (self.card / "busy.flag").unlink()
        self.assertIn("foto.bin", self.ftp.nlst())
        (self.card / "recording.flag").write_text("recording")
        with self.assertRaises(ftplib.error_temp):
            self.ftp.retrbinary("RETR active/frame.part", lambda data: None)
        image = bytearray()
        self.ftp.retrbinary("RETR active/frame.jpg", image.extend)
        self.assertEqual(image, b"complete")
        (self.card / "recording.flag").unlink()
        image.clear()
        self.ftp.retrbinary("RETR active/frame.part", image.extend)
        self.assertEqual(image, b"partial")

    def test_two_control_sessions_and_one_data_transfer(self):
        second = self.connect()
        try:
            self.ftp.voidcmd("TYPE I")
            data = self.ftp.transfercmd("RETR large.bin")
            with self.assertRaises(ftplib.error_temp):
                second.nlst()
            data.close()
            try:
                self.ftp.voidresp()
            except ftplib.error_temp:
                pass
            self.assertIn("foto.bin", second.nlst())
            self.assertTrue(self.ftp.voidcmd("NOOP").startswith("200"))
            self.assertTrue(second.voidcmd("NOOP").startswith("200"))
            extra = ftplib.FTP(timeout=5)
            with self.assertRaises(ftplib.error_temp):
                extra.connect("127.0.0.1", self.port)
            extra.close()
        finally:
            second.quit()

    def test_disable_drains_transfer_closes_idle_sessions_and_reopens(self):
        idle = self.connect()
        marker = self.card / "ftp-disabled.flag"
        try:
            self.ftp.voidcmd("TYPE I")
            data = self.ftp.transfercmd("RETR drain.bin")
            marker.write_text("off")
            time.sleep(0.08)
            with self.assertRaises((ftplib.error_temp, EOFError, OSError)):
                idle.voidcmd("NOOP")
            with self.assertRaises(OSError):
                connection = socket.create_connection(("127.0.0.1", self.port), timeout=1)
                connection.close()
            content = bytearray()
            with data:
                while True:
                    chunk = data.recv(8192)
                    if not chunk:
                        break
                    content.extend(chunk)
            self.assertEqual(content, bytes(range(256)) * 2048)
            self.assertTrue(self.ftp.voidresp().startswith("226"))
            with self.assertRaises((ftplib.error_temp, EOFError, OSError)):
                self.ftp.voidcmd("NOOP")
            marker.unlink()
            time.sleep(0.1)
            fresh = self.connect()
            try:
                self.assertIn("foto.bin", fresh.nlst())
            finally:
                fresh.quit()
        finally:
            marker.unlink(missing_ok=True)
            idle.close()

    def test_passive_data_peer_must_match_control_peer(self):
        self.ftp.voidcmd("TYPE I")
        host, port = ftplib.parse227(self.ftp.sendcmd("PASV"))
        attacker = socket.socket()
        try:
            try:
                attacker.bind(("127.0.0.2", 0))
            except OSError:
                self.skipTest("Endereço de loopback alternativo não disponível.")
            attacker.settimeout(5)
            attacker.connect((host, port))
            self.assertTrue(self.ftp.sendcmd("RETR foto.bin").startswith("150"))
            with socket.create_connection((host, port), timeout=5) as data:
                content = bytearray()
                while True:
                    chunk = data.recv(8192)
                    if not chunk:
                        break
                    content.extend(chunk)
            self.assertEqual(content, self.binary)
            self.ftp.voidresp()
            try:
                self.assertEqual(attacker.recv(1), b"")
            except ConnectionResetError:
                pass
        finally:
            attacker.close()


if __name__ == "__main__":
    unittest.main()
