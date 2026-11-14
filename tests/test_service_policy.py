from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ServicePolicyTests(unittest.TestCase):
    def test_real_codec_and_drain_states(self):
        compiler = Path(os.environ.get("TCC_EXE", ROOT / "verification/tools/tcc/tcc.exe"))
        if not compiler.is_file():
            self.skipTest("Defina TCC_EXE para executar o mesmo código C da política de serviços.")
        with tempfile.TemporaryDirectory(prefix="service-check-", dir=ROOT / "verification") as temporary:
            executable = Path(temporary) / "policy.exe"
            saved = Path(temporary) / "preferences.bin"
            compile_result = subprocess.run([
                str(compiler), "-o", str(executable), str(ROOT / "tests/service_policy_fixture.c")
            ], capture_output=True, text=True)
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            result = subprocess.run([str(executable), str(saved)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(saved.stat().st_size, 48 * 4)

    def test_services_are_enforced_in_firmware(self):
        main = (ROOT / "src/main.cpp").read_text(encoding="utf-8")
        ftp = (ROOT / "src/ftp_service.h").read_text(encoding="utf-8")
        self.assertIn('ServiceLease webAccess(RC_SERVICE_WEB_SD', main)
        self.assertIn('ServiceLease permission(RC_SERVICE_OTA', main)
        self.assertIn('preferences.putUInt("svc_cfg", requested)', main)
        self.assertIn('rc_service_load(storedServices)', main)
        self.assertIn('rcftp_close(ftpListener)', ftp)
        self.assertIn('context->leased || serviceEnabled(RC_SERVICE_FTP)', ftp)
        self.assertNotIn('MDNS.end(', ftp)
        self.assertNotIn('WiFi.mode(WIFI_OFF)', main)
        self.assertEqual(main.count('webSdJobs.fetch_add(1)'), 3)
        self.assertEqual(main.count('webSdJobs.fetch_sub(1)'), 4)
        setting_handler = main.split('esp_err_t configureServices(', 1)[1].split('esp_err_t statusHandler(', 1)[0]
        for forbidden in ['esp_camera_deinit', 'WiFi.disconnect', 'Update.abort', 'stopRecording =']:
            self.assertNotIn(forbidden, setting_handler)
        controller = main.split('esp_err_t controlHandler(', 1)[1]
        self.assertLess(controller.index('path == "/api/services"'), controller.index('Guard mutation'))


if __name__ == "__main__":
    unittest.main()
