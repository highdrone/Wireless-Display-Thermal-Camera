# Source-attested firmware image

`ThermalCam-full-flash-at-0x0.bin` is a newly compiled **16 MB merged flash
image**, not a board dump or standalone application. Program it at **0x0** only.

- SHA-256: `4b634de79c861fef7a327fd4fe07763b923ab27187e39c75395c00cdf49b73c7`
- Length: **16,777,216 bytes**.
- Source commit: `11aa27458bc893646d41af18e95cc3110f438fe2` (adds camera
  standby: the camera turns back on by itself when its wireless screen does).
- `ThermalCam/` source-tree hash: `7948e36c48fa248479c15f1a96a73a462f80f2af`.
- Core **3.3.12**, GFX **1.6.8**, OPI PSRAM, 16 MB flash, 3 MB app/9.9 MB FAT
  partition scheme, hardware USB CDC and CDC-on-boot enabled.
- Actual application SHA-256:
  `a2e420a55be4dd18fce5dfa88a87786c0cae51f95ab94a1e006d98073d9fa2b2`.
- Actual ELF SHA-256 (also attested by the application descriptor):
  `5e84e245f17d176afebc3f505cfd9080a0ac10022b1789fcc4c9d238d8315987`.
- Built on Linux x86_64 with Arduino CLI 1.5.1, following [BUILDING.md](BUILDING.md).

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
same Linux x86_64 host. This does not certify cross-host/future reproducibility.
See [BUILDING.md](BUILDING.md) for the exact isolated build/release procedure.
Keep ELF/map/logs/build-options private; only the reviewed merged image is tracked.

**Cross-host check.** The same Linux setup rebuilt the previous image's source
(commit `8249337`, built on macOS arm64). Its bootloader and partition table came
out byte-identical. Its application differed only where the Arduino core embeds
the build host's OS name (`linux` instead of `macosx`) and in the addresses after
that string. With only that label overridden as a diagnostic, every byte matched
except the embedded ELF hash and the image digest that covers it. So both hosts
compile the same code from the same pinned inputs, but an image built on a
different host OS will not have the same checksum.

Verify offline from the repository root:

```sh
printf '%s  %s\n' \
  '4b634de79c861fef7a327fd4fe07763b923ab27187e39c75395c00cdf49b73c7' \
  'firmware/ThermalCam-full-flash-at-0x0.bin' | shasum -a 256 -c -
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest discover -s tests -v
python3 -B tools/privacy_check.py
```

On Linux, use `sha256sum -c -` instead of `shasum -a 256 -c -`.

## Limits

**No hardware test, board access, flashing or serial session was performed with
this image.** The wireless-screen and camera standby code was checked in a host
simulation only. The binary includes the parser hardening and disabled sensor-ID/thermal
serial logging defaults; it still broadcasts unauthenticated, unencrypted
ESP-NOW when the default wireless mode is used. Disable `WIRELESS_SCREEN` and
rebuild for sensitive use. SD captures remain unencrypted.

Raw/printable/UTF-16 offline privacy checks found no local home path, private
identity/contact, configured secret, token or device MAC in the new flashed
image; the only build paths in it are the mapped `/toolchain/...` prefix. That
is a bounded heuristic observation, not proof of absence. Entropy
classification and final publication review are separate gates; a compile/hash
is not security certification. Retain notices and source/rebuild access described
in `THIRD_PARTY.md`; the whole binary is not solely MIT and no exhaustive binary
license-compliance certification is claimed.

The precompiled SDK app descriptor says `arduino-lib-builder`, `6671d0b`, and
September 16, 2026; these are inherited SDK metadata, **not** the thermal source
identity or October 5 compile date. Its embedded ELF SHA-256 does match the real
application build. Use the external manifest for source correspondence.

The previous source-attested images (SHA-256
`8704ae77ceb5193a72d2bee500bdec6fb3fcaa182deac8c13cdbececec962acf`, source
commit `55a4370d302cd2e7d74ab8f39f7849c4b1f6929e`, and SHA-256
`a2c0a4e43a25186448d71794e88cacdbf392497f3301813ef07890e6a023c930`, source
commit `824933755386262ea5472ff1e908bb8ddc78c97e`) were preserved privately
before replacement. The image before that (SHA-256
`8cf0bc53a25bc20b4bbe698fb0024ef552ef7dff8ba177f0c8a7b527b96fef49`,
1,155,072 bytes) still has **unproven** historical source/dependency
correspondence; no historical bitwise reproduction is claimed. Git history was
not rewritten. Replacement neither removes nor validates older distributed copies.
