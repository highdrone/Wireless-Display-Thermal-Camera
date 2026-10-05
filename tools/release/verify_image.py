"""Independent check of a merged image against the real build outputs: every
flash range matches its input, everything else is 0xFF, the partition table
MD5, ESP image checksums/digests, blank NVS/app1/FAT/coredump, and the app
descriptor's ELF SHA-256. Prints the facts the manifest records, as JSON.

Usage: python3 tools/release/verify_image.py BUILD_DIR CORE_DIR/tools/partitions/boot_app0.bin
"""
import hashlib, json, struct, sys
from pathlib import Path
B = Path(sys.argv[1]); core_boot_app0 = Path(sys.argv[2])
sha = lambda b: hashlib.sha256(b).hexdigest()
merged = (B / "ThermalCam.ino.merged.bin").read_bytes()
elf = (B / "ThermalCam.ino.elf").read_bytes()
assert len(merged) == 16 * 1024 * 1024
lines = (B / "flash_args").read_text().split("\n")
parts = [l.split() for l in lines[1:] if l.strip()]
roles = {"ThermalCam.ino.bootloader.bin": "bootloader", "ThermalCam.ino.partitions.bin": "partition_table", "boot_app0.bin": "boot_app0", "ThermalCam.ino.bin": "application"}
comps, at = [], 0
for off, name in parts:
    off = int(off, 16); data = (B / name).read_bytes()
    assert merged[at:off] == b"\xff" * (off - at), name
    assert merged[off:off + len(data)] == data, name
    comps.append({"role": roles[name], "build_file": name, "flash_offset": hex(off), "bytes": len(data), "sha256": sha(data)})
    at = off + len(data)
assert merged[at:] == b"\xff" * (len(merged) - at)
assert (B / "boot_app0.bin").read_bytes() == core_boot_app0.read_bytes()
# partition table
pt = merged[0x8000:0x8000 + 3072]; p, partitions = 0, []
while pt[p:p + 2] == b"\xaa\x50":
    _, kind, sub, off, size, label, flags = struct.unpack_from("<HBBII16sI", pt, p)
    entry = {"label": label.split(b"\0")[0].decode(), "type": kind, "subtype": sub, "offset": hex(off), "bytes": size, "flags": flags}
    region = merged[off:off + size]
    if entry["label"] in ("nvs", "app1", "ffat", "coredump"):
        assert region == b"\xff" * size, entry["label"]
        entry["all_ff"] = True; entry["sha256"] = sha(region)
    partitions.append(entry); p += 32
assert pt[p:p + 2] == b"\xeb\xeb" and pt[p + 16:p + 32] == hashlib.md5(pt[:p]).digest()
# ESP image checksum + appended SHA-256, and the app descriptor
def check_image(img):
    assert img[0] == 0xE9 and struct.unpack_from("<H", img, 12)[0] == 9
    a, c = 24, 0xEF
    for _ in range(img[1]):
        _, n = struct.unpack_from("<II", img, a); a += 8
        for v in img[a:a + n]: c ^= v
        a += n
    ck = a + (15 - a % 16)
    assert img[ck] == c and img[23] == 1
    assert img[ck + 1:ck + 33] == hashlib.sha256(img[:ck + 1]).digest() and ck + 33 == len(img)
app = (B / "ThermalCam.ino.bin").read_bytes(); boot = (B / "ThermalCam.ino.bootloader.bin").read_bytes()
check_image(app); check_image(boot)
d = app[32:288]
assert struct.unpack_from("<I", d)[0] == 0xABCD5432
assert d[144:176].hex() == sha(elf), "descriptor ELF hash"
desc = {k: d[a:b].split(b"\0")[0].decode() for k, a, b in (("version", 16, 48), ("project_name", 48, 80), ("time", 80, 96), ("date", 96, 112), ("idf_version", 112, 144))}
print(json.dumps({"merged_sha256": sha(merged), "merged_bytes": len(merged), "elf": {"bytes": len(elf), "sha256": sha(elf)},
                  "components": comps, "partitions": partitions, "app_descriptor": desc}, indent=1))
