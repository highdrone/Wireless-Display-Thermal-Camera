# Source-attested firmware image

`ThermalCam-full-flash-at-0x0.bin` is a newly compiled **16 MB merged flash
image**, not a board dump or standalone application. Program it at **0x0** only.

- SHA-256: `a2c0a4e43a25186448d71794e88cacdbf392497f3301813ef07890e6a023c930`
- Length: **16,777,216 bytes**.
- Source commit: `824933755386262ea5472ff1e908bb8ddc78c97e`.
- `ThermalCam/` source-tree hash: `42e38bc67e21dac0417539ebf333fe9203ad3f26`.
- Core **3.3.12**, GFX **1.6.8**, OPI PSRAM, 16 MB flash, 3 MB app/9.9 MB FAT
  partition scheme, hardware USB CDC and CDC-on-boot enabled.
- Actual application SHA-256:
  `5aed0a593677b9bdaab176e03188cf5f81022d7d7228ccbb69c4a7258f5c653b`.
- Actual ELF SHA-256 (also attested by the application descriptor):
  `3d9a2b8f7131e1532d5099e24c086478ab92c19a14e0cd16e7e739c2588bd8a6`.

`manifest.json` records full source-file hashes, relevant dependency pins/hashes,
FQBN, build recipe, component hashes/offsets and partition checks. Its source
commit precedes the manifest commit, avoiding circular self-reference. The
source did not change between that source commit and this binary build.

The merged inputs were verified byte-for-byte against the real compile outputs:
bootloader at `0x0`, partition table at `0x8000`, SDK OTA initialization
`boot_app0.bin` at `0xe000`, and freshly compiled app at `0x10000`. Every other
byte is `0xFF`; NVS (`0x9000–0xdfff`), second app, FAT and coredump partitions
are entirely blank. **Flashing the full image replaces device settings/data.**
Never substitute a board dump or real capture for a release build input.

Two complete builds with separate new build directories/caches and fixed
`SOURCE_DATE_EPOCH` produced identical ELF, application and merged bytes on the
same macOS arm64 host. This does not certify cross-host/future reproducibility.
See [BUILDING.md](BUILDING.md) for the exact isolated build/release procedure.
Keep ELF/map/logs/build-options private; only the reviewed merged image is tracked.

Verify offline from the repository root:

```sh
printf '%s  %s\n' \
  'a2c0a4e43a25186448d71794e88cacdbf392497f3301813ef07890e6a023c930' \
  'firmware/ThermalCam-full-flash-at-0x0.bin' | shasum -a 256 -c -
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest discover -s tests -v
python3 -B tools/privacy_check.py
```

## Limits

**No hardware test, board access, flashing or serial session was performed.**
The binary includes the current parser hardening and disabled sensor-ID/thermal
serial logging defaults; it still broadcasts unauthenticated, unencrypted
ESP-NOW when the default wireless mode is used. Disable `WIRELESS_SCREEN` and
rebuild for sensitive use. SD captures remain unencrypted.

Raw/printable/UTF-16 offline privacy checks found no local home path, private
identity/contact, configured secret, token or device MAC in the new flashed
image. That is a bounded heuristic observation, not proof of absence. Entropy
classification and final publication review are separate gates; a compile/hash
is not security certification. Retain notices and source/rebuild access described
in `THIRD_PARTY.md`; the whole binary is not solely MIT and no exhaustive binary
license-compliance certification is claimed.

The precompiled SDK app descriptor says `arduino-lib-builder`, `6671d0b`, and
September 16, 2026; these are inherited SDK metadata, **not** the thermal source
identity or October 3 compile date. Its embedded ELF SHA-256 does match the real
application build. Use the external manifest for source correspondence.

The former current image (SHA-256
`8cf0bc53a25bc20b4bbe698fb0024ef552ef7dff8ba177f0c8a7b527b96fef49`,
1,155,072 bytes) was preserved privately before replacement. Its metadata was
checked, but historical source/dependency correspondence remains **unproven**;
no historical bitwise reproduction is claimed. Git history was not rewritten.
Replacement neither removes nor validates older distributed copies.
