# Source-attested firmware image

`ThermalCam-full-flash-at-0x0.bin` is a newly compiled **16 MB merged flash
image**, not a board dump or standalone application. Program it at **0x0** only.

- SHA-256: `59bc5238dd1e37d50f26bf4988154b090dce8d1ca72f4395041c2e107aea6160`
- Length: **16,777,216 bytes**.
- Source commit: `259bf17c67609b251c08e4be7969831bfb50d698` (the wireless
  screen lays an HT-HC33 camera head's thermal picture over its color picture).
- `ThermalCam/` source-tree hash: `9c141e8f69f2c48503ffb0d86e8a5cc65702b62e`.
- Core **3.3.12**, GFX **1.6.8**, OPI PSRAM, 16 MB flash, 3 MB app/9.9 MB FAT
  partition scheme, hardware USB CDC and CDC-on-boot enabled.
- Actual application SHA-256:
  `71a30772fa57320cd8543a8cfef8a5aea671e4f3a9711a4c5a7a0706b7ae1b98`.
- Actual ELF SHA-256 (also attested by the application descriptor):
  `09e1aa6504137e2b6fe69ab75a5e70cb88972557eb63b9190bcb241c6edb4321`.
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

## HT-HC33 camera head image

`ThermalCamHead-HT-HC33-full-flash-at-0x0.bin` is the **8 MB merged flash
image** for the Heltec HT-HC33 thermal + color camera head. Program it at
**0x0** on the HT-HC33 only; [head-manifest.json](head-manifest.json) records
it the same way.

- SHA-256: `99dd5ad72035dbb59df08124b6c138360ea1afbb47eb6a780fba377545340bc8`
- Length: **8,388,608 bytes**.
- Source commit: `259bf17c67609b251c08e4be7969831bfb50d698`;
  `ThermalCamHead/` source-tree hash: `f1e424557e0aa7063a3063a3a020b0e50315163b`.
- Core **3.3.12** (its precompiled esp32-camera and esp_jpeg), OPI PSRAM, 8 MB
  flash, default 8 MB partition scheme (two 3.2 MB app slots, 1.5 MB SPIFFS),
  USB serial through the board's CP2102 (no CDC on boot).
- Actual application SHA-256:
  `bc1a254e7a1f3430015d12e280dcdffeeab0cbf944169263e9a7e1e804a55d5f`.
- Actual ELF SHA-256: `d875bf1c93377c52de8323ce5dfbcd1660eaba0b2b8e5018b72fb0c2f4522617`.
- Board options:
  `esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=default,FlashSize=8M,PartitionScheme=default_8MB,PSRAM=opi`.

It was verified the same way as the screen image (every range against its
build input, everything else `0xFF`, blank NVS, second app, SPIFFS and coredump),
and two builds in new directories with new caches were byte-identical. Its
pins come from Heltec's Arduino board definition and datasheet excerpts; it has
not run on an HT-HC33.

Verify offline from the repository root:

```sh
printf '%s  %s\n' \
  '59bc5238dd1e37d50f26bf4988154b090dce8d1ca72f4395041c2e107aea6160' \
  'firmware/ThermalCam-full-flash-at-0x0.bin' | shasum -a 256 -c -
printf '%s  %s\n' \
  '99dd5ad72035dbb59df08124b6c138360ea1afbb47eb6a780fba377545340bc8' \
  'firmware/ThermalCamHead-HT-HC33-full-flash-at-0x0.bin' | shasum -a 256 -c -
PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest discover -s tests -v
python3 -B tools/privacy_check.py
```

On Linux, use `sha256sum -c -` instead of `shasum -a 256 -c -`.

## Limits

**No hardware test, board access, flashing or serial session was performed with
this image.** The wireless-screen, camera standby and color-overlay code was
checked in a host simulation only. The binary includes the parser hardening and disabled sensor-ID/thermal
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
identity or October 9 compile date. Its embedded ELF SHA-256 does match the real
application build. Use the external manifest for source correspondence.

The previous source-attested images (SHA-256
`5b2251a666fddb0c136540dcd0da6f7dd3d9bc81a2e22928c26c6649b6e78e5d`, source
commit `24585557e60ae37f7fe9b0d6b28d6be6492b9aa1`; SHA-256
`4b634de79c861fef7a327fd4fe07763b923ab27187e39c75395c00cdf49b73c7`, source
commit `11aa27458bc893646d41af18e95cc3110f438fe2`; SHA-256
`8704ae77ceb5193a72d2bee500bdec6fb3fcaa182deac8c13cdbececec962acf`, source
commit `55a4370d302cd2e7d74ab8f39f7849c4b1f6929e`, and SHA-256
`a2c0a4e43a25186448d71794e88cacdbf392497f3301813ef07890e6a023c930`, source
commit `824933755386262ea5472ff1e908bb8ddc78c97e`) were preserved privately
before replacement. The image before that (SHA-256
`8cf0bc53a25bc20b4bbe698fb0024ef552ef7dff8ba177f0c8a7b527b96fef49`,
1,155,072 bytes) still has **unproven** historical source/dependency
correspondence; no historical bitwise reproduction is claimed. Git history was
not rewritten. Replacement neither removes nor validates older distributed copies.
