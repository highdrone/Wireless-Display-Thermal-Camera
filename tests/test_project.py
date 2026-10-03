import importlib.util
import json
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("privacy", ROOT / "tools/privacy_check.py")
privacy = importlib.util.module_from_spec(spec)
spec.loader.exec_module(privacy)

class ProjectTests(unittest.TestCase):
    def test_native_bmp_and_palettes(self):
        compiler = shutil.which("c++") or shutil.which("g++")
        self.assertIsNotNone(compiler, "Install a C++11 compiler for native tests")
        with tempfile.TemporaryDirectory(prefix="thermal-tests-") as folder:
            binary = Path(folder) / "native-test"
            subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(ROOT / "tests/native.cpp"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_privacy_detector(self):
        fixtures = [(b"/" + b"Users/fixture/private", "home_path"),
                    (b"192" + b".168.42.42", "private_ip"),
                    (b"ssid" + b'= "synthetic-network"', "configured_credential"),
                    (b"ghp_" + b"A" * 36, "credential_token"),
                    (b"fixture" + b"@example.invalid", "email"),
                    (b"12:34:" + b"56:78:9a:bc", "device_mac")]
        for data, expected in fixtures:
            with self.subTest(kind=expected):
                found = privacy.findings(data)
                self.assertIn(expected, [x[0] for x in found])
                self.assertTrue(all(len(x[1]) == 64 for x in found))
        self.assertFalse(privacy.findings(b"ff:ff:ff:ff:ff:ff"))
        self.assertFalse(privacy.findings(b"contributor" + b"@users.noreply.github.com"))

    def test_preserved_defaults_and_private_logging(self):
        config = (ROOT / "ThermalCam/config.h").read_text()
        for name, value in {"THERMAL_SDA": "15", "THERMAL_SCL": "14", "WIRELESS_SCREEN": "true", "WIRELESS_CHANNEL": "1", "LOG_SENSOR_SERIAL": "false", "LOG_THERMAL_STATS": "false"}.items():
            self.assertRegex(config, rf"(?m)^#define {name} {value}(?:\s|$)")
        sketch = (ROOT / "ThermalCam/ThermalCam.cpp").read_text()
        self.assertRegex(sketch, r"(?s)#if LOG_SENSOR_SERIAL.*?serial %04X.*?#else.*?#endif")
        self.assertRegex(sketch, r"(?s)#if LOG_THERMAL_STATS.*?frames/s.*?#endif")
        self.assertIn("parseBmpHeader(hdr, sizeof(hdr), f.size(), layout)", sketch)

    def test_generated_and_private_files_ignored(self):
        names = [".env", ".env.production", "local_config.h", "config.local.json", "secrets.json", "private/results.json", "backups/history.bundle", "device.dump", "capture.bmp", "frame.csv", "output/firmware.bin", "build-verify/app.elf", "new-firmware.bin", "tmp/log.txt"]
        result = subprocess.run(["git", "check-ignore", "--no-index", "--stdin"], cwd=ROOT, input="\n".join(names) + "\n", text=True, capture_output=True)
        self.assertEqual(set(result.stdout.splitlines()), set(names))

    def test_source_attested_manifest(self):
        manifest = json.loads((ROOT / "firmware/manifest.json").read_text())
        artifact = ROOT / "firmware" / manifest["file"]
        import hashlib
        self.assertEqual(hashlib.sha256(artifact.read_bytes()).hexdigest(), manifest["sha256"])
        self.assertEqual(artifact.stat().st_size, manifest["bytes"])
        self.assertEqual(manifest["flash_offset"], "0x0")
        self.assertRegex(manifest["source_commit"], r"^[0-9a-f]{40}$")
        self.assertEqual(manifest["schema_version"], 2)
        self.assertFalse(manifest["verification"]["hardware_tested"])
        source_paths = set()
        for entry in manifest["source_tree"]["files"]:
            source_paths.add(entry["path"])
            data = (ROOT / entry["path"]).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry["sha256"])
            self.assertEqual(len(data), entry["bytes"])
            git_blob = hashlib.sha1(b"blob " + str(len(data)).encode() + b"\0" + data).hexdigest()
            self.assertEqual(git_blob, entry["git_blob"])
        actual = subprocess.check_output(["git", "ls-files", "ThermalCam"], cwd=ROOT, text=True).splitlines()
        self.assertEqual(source_paths, set(actual))
        # Works with depth-one CI: it does not need the preceding commit object.
        tree = subprocess.check_output(["git", "rev-parse", "HEAD:ThermalCam"], cwd=ROOT, text=True).strip()
        self.assertEqual(tree, manifest["source_tree"]["git_tree"])

    def test_exact_upstream_notices(self):
        import hashlib
        notices = json.loads((ROOT / "LICENSES/upstream-manifest.json").read_text())
        for entry in notices["files"]:
            data = (ROOT / "LICENSES" / entry["file"]).read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), entry["sha256"])
            self.assertEqual(len(data), entry["bytes"])
        for name in privacy.REVIEWED_UPSTREAM_NOTICES:
            data = (ROOT / name).read_bytes()
            self.assertTrue(privacy.reviewed_upstream_notice(name, data))
            self.assertFalse(privacy.reviewed_upstream_notice(name, data + b"modified"))
            self.assertFalse(privacy.reviewed_upstream_notice("LICENSES/unreviewed.txt", data))
            self.assertTrue(privacy.findings(data))

if __name__ == "__main__":
    unittest.main()
