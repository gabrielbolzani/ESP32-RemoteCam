import importlib.util
from pathlib import Path
import unittest
import runpy
from types import SimpleNamespace
from unittest.mock import patch, Mock

ROOT = Path(__file__).resolve().parents[1]
specification = importlib.util.spec_from_file_location("controlar", ROOT / "controlar.py")
controlar = importlib.util.module_from_spec(specification)
specification.loader.exec_module(controlar)


class ToolTests(unittest.TestCase):
    def test_upload_has_explicit_port(self):
        with patch.object(controlar, "run_pio_command", return_value=0) as runner:
            self.assertEqual(controlar.execute("upload", "COM5"), 0)
            runner.assert_called_once_with(["run", "-e", "esp32cam", "-t", "upload", "--upload-port", "COM5"])

    def test_failure_is_returned(self):
        with patch.object(controlar.shutil, "which", return_value="pio.exe"), patch.object(controlar.subprocess, "run", return_value=Mock(returncode=7)) as runner, patch("builtins.print"):
            self.assertEqual(controlar.run_pio_command(["run"]), 7)
            self.assertFalse(runner.call_args.kwargs["shell"])
            self.assertEqual(runner.call_args.kwargs["cwd"], ROOT)

    def test_compile_failure_prevents_upload(self):
        with patch.object(controlar.sys, "argv", ["controlar.py"]), patch("builtins.input", side_effect=["3", "6"]), patch.object(controlar, "execute", return_value=1) as runner, patch("builtins.print"):
            self.assertEqual(controlar.main(), 0)
            runner.assert_called_once_with("compile", None)

    def test_ui_resolution_mapping(self):
        html = (ROOT / "web/index.html").read_text(encoding="utf-8")
        self.assertIn("[8,'VGA · 640×480']", html)
        self.assertIn("[13,'UXGA · 1600×1200']", html)
        self.assertNotIn("innerHTML", html)

    def test_no_automatic_format_on_mount(self):
        source = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        mount = source.split("bool mountStorage() {", 1)[1].split("bool writeAll(", 1)[0]
        self.assertNotIn("f_mkfs", mount)
        self.assertNotIn("f_fdisk", mount)
        self.assertIn('confirmation"].as<String>() != "FORMATAR"', source)


    def test_no_login_in_firmware_or_ui(self):
        firmware = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        ui = (ROOT / "web/index.html").read_text(encoding="utf-8")
        for value in ["/api/login", "sessionToken", "adminPassword", "X-CSRF-Token"]:
            self.assertNotIn(value, firmware)
            self.assertNotIn(value, ui)
        self.assertNotIn('type="password"', ui)

    def test_version_is_a_single_source(self):
        version = (ROOT / "VERSION").read_text(encoding="utf-8").strip()
        self.assertRegex(version, r"^[0-9]+(?:\.[0-9]+)+(?:[A-Za-z][A-Za-z0-9]*)?(?:-[A-Za-z0-9.-]+)?$")
        firmware = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        self.assertIn("static const char *VERSION = FIRMWARE_VERSION;", firmware)
        self.assertNotIn(version, firmware)

    def test_version_change_propagates_without_touching_sources(self):
        original_read = Path.read_text
        writes = {}

        def read_text(path, *args, **kwargs):
            if path == ROOT / "VERSION":
                return "9.8.7\n"
            return original_read(path, *args, **kwargs)

        def write_text(path, content, *args, **kwargs):
            writes[path] = content
            return len(content)

        environment = SimpleNamespace(subst=lambda name: str(ROOT))
        with patch.object(Path, "read_text", read_text), patch.object(Path, "write_text", write_text):
            runpy.run_path(
                str(ROOT / "scripts/embed_ui.py"),
                init_globals={"Import": lambda name: None, "env": environment},
            )
        self.assertIn('#define FIRMWARE_VERSION "9.8.7"', writes[ROOT / "src/firmware_version.h"])
        self.assertIn("Firmware 9.8.7", writes[ROOT / "src/web_ui.h"])
        self.assertNotEqual((ROOT / "VERSION").read_text(encoding="utf-8").strip(), "9.8.7")

    def test_monitor_routes_stay_on_original_port(self):
        firmware = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        ui = (ROOT / "web/index.html").read_text(encoding="utf-8")
        self.assertIn('if (path == "/stream") return streamHandler(request);', firmware)
        self.assertIn('if (path == "/snapshot" || path == "/raw") return snapshotHandler(request);', firmware)
        self.assertNotIn("302 Found", firmware)
        self.assertNotIn(":81", ui)
        self.assertNotIn(":82", ui)
        self.assertNotIn("/api/stream/stop", ui)
        self.assertIn("streamClients.fetch_add(1)", firmware)
        self.assertIn("request->client.connected()", firmware)

    def test_chip_temperature_replaces_snr(self):
        firmware = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        ui = (ROOT / "web/index.html").read_text(encoding="utf-8")
        self.assertIn("temperatureRead()", firmware)
        self.assertIn("chip_temperature_c", firmware)
        self.assertNotIn("snr", firmware.lower())
        self.assertNotIn("snr", ui.lower())
        self.assertIn('href="/gallery"', ui)

    def test_session_folder_and_power_loss_metadata(self):
        firmware = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        self.assertIn('String session = "/timelapse/sessao-"', firmware)
        self.assertIn('mkdir(folder.c_str(), 0775)', firmware)
        self.assertIn('recoverInterruptedSession()', firmware)
        self.assertIn('document["state"] = "interrupted"', firmware)
        self.assertIn('Guard mutation(commandMutex', firmware)

    def test_ota_prefetch_preserves_binary_zeroes(self):
        transport = (ROOT / "src/http_transport.h").read_text(encoding="utf-8")
        self.assertIn('std::vector<uint8_t> bodyPrefix;', transport)
        self.assertIn('bodyPrefix.assign(receivedBytes + headerEnd + 4', transport)
        self.assertNotIn('bodyPrefix = received.substring', transport)

    def test_folder_jobs_have_explicit_confirmation_and_protection(self):
        firmware = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        actions = (ROOT / "web/folder_actions.js").read_text(encoding="utf-8")
        self.assertIn('StorageOperation::DeleteTree', firmware)
        self.assertIn('StorageOperation::Rename', firmware)
        self.assertIn('body["confirmation"].as<String>() != "EXCLUIR"', firmware)
        self.assertIn('path.equalsIgnoreCase("/timelapse")', firmware)
        self.assertIn('state.maintenance = true', firmware)
        self.assertIn('inspectTree(command.path', firmware)
        self.assertIn('recursive:true', actions)
        self.assertIn("input.pattern = 'EXCLUIR'", actions)
        self.assertNotIn('innerHTML', actions)

    def test_zip_is_not_a_renamed_tar(self):
        firmware = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        self.assertIn('application/zip', firmware)
        self.assertIn('rc_zip_finish', firmware)
        self.assertNotIn('application/x-tar', firmware)
        self.assertNotIn('tarDirectory', firmware)
        self.assertIn('files.fingerprint != central.fingerprint', firmware)

    def test_ram_used_and_free_are_distinct(self):
        firmware = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        page = (ROOT / "web/index.html").read_text(encoding="utf-8")
        self.assertIn('ESP.getHeapSize()', firmware)
        self.assertIn('document["heap_used_pct"]', firmware)
        self.assertIn('RAM utilizada', page)
        self.assertIn('id="heap-free"', page)
        self.assertIn("size(data.heap_free)+' livres'", page)

    def test_ftp_uses_existing_storage_without_write_operations(self):
        adapter = (ROOT / "src/ftp_service.h").read_text(encoding="utf-8")
        firmware = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        self.assertIn('FTP_CONTROL_PORT = 21', adapter)
        self.assertIn('FTP_MAX_SESSIONS = 2', adapter)
        self.assertIn('FTP_PASSIVE_FIRST = 50000', adapter)
        self.assertIn('state.download = true', adapter)
        self.assertIn('context->leased', adapter)
        self.assertIn('Guard disk(sdMutex)', adapter)
        self.assertIn('recordingBusy(state)', adapter)
        self.assertIn('"rb"', adapter)
        for forbidden in ['"wb"', 'fwrite(', 'unlink(', 'rename(', 'f_mkfs(', 'sdmmc_host_init(']:
            self.assertNotIn(forbidden, adapter)
        self.assertIn('serviceFtpClients();', firmware)
        self.assertIn('startFtpServer();', firmware)

if __name__ == "__main__":
    unittest.main()
