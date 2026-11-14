"""Executa o mesmo encoder C usado pelo firmware e lê os ZIPs com zipfile."""
from pathlib import Path
import os
import struct
import subprocess
import tempfile
import unittest
import zipfile

ROOT = Path(__file__).resolve().parents[1]


class ZipEncoderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = Path(os.environ.get("TCC_EXE", ROOT / "verification/tools/tcc/tcc.exe"))
        if not compiler.is_file():
            raise unittest.SkipTest("Defina TCC_EXE para o compilador C portátil de testes.")
        (ROOT / "verification").mkdir(exist_ok=True)
        cls.directory = tempfile.TemporaryDirectory(prefix="zip-check-", dir=ROOT / "verification")
        cls.folder = Path(cls.directory.name)
        executable = cls.folder / "zip-fixture.exe"
        subprocess.run([str(compiler), "-o", str(executable), str(ROOT / "tests/zip_fixture.c")], check=True, capture_output=True, text=True)
        cls.sample = cls.folder / "sample.zip"
        cls.many = cls.folder / "many.zip"
        cls.large = cls.folder / "large-header.bin"
        subprocess.run([str(executable), str(cls.sample), str(cls.many), str(cls.large)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def test_binary_unicode_empty_directories_and_crc(self):
        with zipfile.ZipFile(self.sample) as archive:
            self.assertIsNone(archive.testzip())
            self.assertIn("sessao/vazia/", archive.namelist())
            self.assertEqual(archive.read("sessao/sub/foto.jpg"), bytes([255,216,0,1,2,255,0,217]))
            self.assertEqual(archive.read("sessao/legenda-ç.txt"), b"123456789")
            for item in archive.infolist():
                self.assertEqual(item.compress_type, zipfile.ZIP_STORED)
                self.assertTrue(item.flag_bits & 8)
                self.assertTrue(item.flag_bits & 2048)

    def test_zip64_entry_count(self):
        with zipfile.ZipFile(self.many) as archive:
            self.assertEqual(len(archive.infolist()), 65536)
            self.assertEqual(archive.read("sessao/f65535"), b"")
            self.assertIsNone(archive.testzip())

    def test_zip64_sizes_and_offsets_do_not_truncate(self):
        data = self.large.read_bytes()
        self.assertEqual(data[:4], b"PK\x01\x02")
        name_size, extra_size = struct.unpack_from("<HH", data, 28)
        extra = data[46 + name_size:46 + name_size + extra_size]
        kind, length, uncompressed, compressed, offset = struct.unpack("<HHQQQ", extra)
        self.assertEqual((kind, length), (1, 24))
        self.assertEqual(uncompressed, (1 << 33) + 123)
        self.assertEqual(compressed, uncompressed)
        self.assertEqual(offset, (1 << 34) + 456)


if __name__ == "__main__":
    unittest.main()
