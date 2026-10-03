"""Offline current-image integrity checks, not a target build/hardware test."""
import hashlib
import json
from pathlib import Path
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]

class FirmwareTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.manifest = json.loads((ROOT / "firmware/manifest.json").read_text())
        cls.data = (ROOT / "firmware" / cls.manifest["file"]).read_bytes()

    def test_merged_components_and_partition_privacy(self):
        m, data = self.manifest, self.data
        self.assertEqual(len(data), 16 * 1024 * 1024)
        self.assertEqual(m["build"]["fqbn"], "esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi")
        self.assertEqual(m["dependencies"]["esp32_core"]["version"], "3.3.12")
        self.assertEqual(m["dependencies"]["arduino_gfx"]["version"], "1.6.8")
        self.assertEqual([int(c["flash_offset"], 16) for c in m["components"]], [0, 0x8000, 0xe000, 0x10000])
        at = 0
        for component in m["components"]:
            off, size = int(component["flash_offset"], 16), component["bytes"]
            self.assertGreaterEqual(off, at)
            self.assertLessEqual(off + size, len(data))
            self.assertEqual(data[at:off], b"\xff" * (off - at))
            self.assertEqual(hashlib.sha256(data[off:off + size]).hexdigest(), component["sha256"])
            at = off + size
        self.assertEqual(data[at:], b"\xff" * (len(data) - at))
        table = next(c for c in m["components"] if c["role"] == "partition_table")
        partdata = data[0x8000:0x8000 + table["bytes"]]
        partitions, at = [], 0
        while partdata[at:at + 2] == b"\xaa\x50":
            self.assertLessEqual(at + 32, len(partdata))
            _, kind, sub, off, size, label, flags = struct.unpack_from("<HBBII16sI", partdata, at)
            partitions.append({"label": label.split(b"\0")[0].decode("ascii"), "type": kind, "subtype": sub, "offset": hex(off), "bytes": size, "flags": flags})
            at += 32
        self.assertEqual(partdata[at:at + 16], b"\xeb\xeb" + b"\xff" * 14)
        self.assertEqual(partdata[at + 16:at + 32], hashlib.md5(partdata[:at]).digest())
        self.assertEqual(partdata[at + 32:], b"\xff" * (len(partdata) - at - 32))
        self.assertEqual(len(partitions), len(m["partitions"]))
        self.assertEqual([p["label"] for p in partitions], ["nvs", "otadata", "app0", "app1", "ffat", "coredump"])
        last_end = 0x9000
        for actual, attested in zip(partitions, m["partitions"]):
            for key, value in actual.items():
                self.assertEqual(value, attested[key])
            off, size = int(actual["offset"], 16), actual["bytes"]
            self.assertGreaterEqual(off, last_end)
            self.assertLessEqual(off + size, len(data))
            last_end = off + size
            if actual["label"] in ("nvs", "app1", "ffat", "coredump"):
                self.assertTrue(attested["all_ff"])
                self.assertEqual(data[off:off + size], b"\xff" * size)
                self.assertEqual(hashlib.sha256(data[off:off + size]).hexdigest(), attested["sha256"])
        app = next(c for c in m["components"] if c["role"] == "application")
        self.assertLessEqual(app["bytes"], next(p for p in partitions if p["label"] == "app0")["bytes"])

    def test_esp_images_and_actual_elf_attestation(self):
        m, data = self.manifest, self.data
        for role in ("bootloader", "application"):
            component = next(c for c in m["components"] if c["role"] == role)
            base, size = int(component["flash_offset"], 16), component["bytes"]
            image = data[base:base + size]
            self.assertEqual(image[0], 0xe9)
            self.assertEqual(struct.unpack_from("<H", image, 12)[0], 9)  # ESP32-S3
            at, checksum = 24, 0xef
            self.assertGreater(image[1], 0)
            for _ in range(image[1]):
                self.assertLessEqual(at + 8, len(image))
                _, count = struct.unpack_from("<II", image, at)
                at += 8
                self.assertLessEqual(at + count, len(image))
                for value in image[at:at + count]:
                    checksum ^= value
                at += count
            checksum_at = at + (15 - at % 16)
            self.assertLess(checksum_at, len(image))
            self.assertEqual(image[checksum_at], checksum)
            end = checksum_at + 1
            self.assertEqual(image[23], 1)
            self.assertEqual(image[end:end + 32], hashlib.sha256(image[:end]).digest())
            self.assertEqual(end + 32, len(image))
            if role == "application":
                desc = image[32:288]
                self.assertEqual(struct.unpack_from("<I", desc)[0], 0xabcd5432)
                self.assertEqual(desc[144:176].hex(), m["build"]["application_elf"]["sha256"])
                for key, a, b in (("version", 16, 48), ("project_name", 48, 80), ("time", 80, 96), ("date", 96, 112), ("idf_version", 112, 144)):
                    self.assertEqual(desc[a:b].split(b"\0")[0].decode("ascii"), m["build"]["app_descriptor"][key])

if __name__ == "__main__":
    unittest.main()
