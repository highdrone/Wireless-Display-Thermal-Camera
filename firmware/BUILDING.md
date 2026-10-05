# Pinned, isolated release build (no upload)

The October 5, 2026 build used Arduino CLI **1.5.1**, Linux x86_64, ESP32 core
**3.3.12**, ESP-IDF **5.5.5**, Xtensa GCC **14.2.0 / esp-14.2.0_20260121**,
esptool **5.3.1**, and Arduino_GFX **1.6.8**. Relevant upstream revisions,
release-archive hashes, source-file hashes, ELF/application/merged-image hashes,
and flash offsets are in `manifest.json`. The previous image was built with the
same recipe on macOS arm64.

**Host OS label:** the Arduino core embeds the build host's OS name
(`ARDUINO_HOST_OS`, e.g. `linux` or `macosx`) in the application. Builds of the
same source on different host operating systems therefore have different
application, ELF and merged-image hashes. The manifest's
`cross_check_previous_source` records how a Linux rebuild of the previous
source compared with the published macOS image.

## 1. Isolate dependencies

Run from the repository root in a POSIX shell. Install Arduino CLI 1.5.1 first.
Choose a **new, private build-state directory**, not an existing Arduino directory.
The commands below write only there; never omit `--config-file` on an install.
They download official dependency packages, not firmware from a device.

```sh
set -eu
umask 077
REPO="$(pwd -P)"
STATE="$REPO/build-release"
mkdir -p "$STATE/arduino-isolated/data" "$STATE/arduino-isolated/downloads" \
  "$STATE/arduino-isolated/user" "$STATE/arduino-isolated/cache" \
  "$STATE/arduino-isolated/home" "$STATE/arduino-isolated/tmp" "$STATE/deps"
CFG="$STATE/arduino-isolated/arduino-cli.yaml"
# Use a fresh STATE; do not overwrite an existing configuration.
test ! -e "$CFG"
cat > "$CFG" <<EOF
board_manager:
  additional_urls:
    - https://espressif.github.io/arduino-esp32/package_esp32_index.json
directories:
  data: $STATE/arduino-isolated/data
  downloads: $STATE/arduino-isolated/downloads
  user: $STATE/arduino-isolated/user
build_cache:
  path: $STATE/arduino-isolated/cache
EOF
# Minimal macOS build environment. Adjust executable PATH on another host
# (Linux: PATH=/usr/bin:/bin, and `sha256sum -c -` for `shasum -a 256 -c -`).
env -i PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin \
  HOME="$STATE/arduino-isolated/home" TMPDIR="$STATE/arduino-isolated/tmp" \
  LANG=C LC_ALL=C arduino-cli --config-file "$CFG" core update-index
env -i PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin \
  HOME="$STATE/arduino-isolated/home" TMPDIR="$STATE/arduino-isolated/tmp" \
  LANG=C LC_ALL=C arduino-cli --config-file "$CFG" core install \
  esp32:esp32@3.3.12 --skip-post-install --skip-pre-uninstall

curl --fail --location \
  'https://codeload.github.com/moononournation/Arduino_GFX/zip/2685a776495be1f9eaf8c572cf876469bcc56585' \
  -o "$STATE/gfx-dependency.zip"
printf '%s  %s\n' \
  '22f7ade4bd0b5efc65380941418932723360780f1498721d06820b779fdd140b' \
  "$STATE/gfx-dependency.zip" | shasum -a 256 -c -
unzip -q "$STATE/gfx-dependency.zip" -d "$STATE/deps"
arduino-cli --config-file "$CFG" core list
```

Verify the core release archive against the manifest/package index and verify
`cores/esp32/esp_arduino_version.h` says 3.3.12. The released core archive is
SHA-256 `87401004f279a206781e368b66098a8383b7cf87cce7811a6afd0163503fa78f`.
Its upstream tag is `94afccf35fb1e401facddbcf9e13bcf7c76a31d8`.
Do not substitute core 3.0.0: GFX 1.6.8 requires newer IDF APIs. Do not patch
unused upstream drivers to make an incompatible dependency set appear to build.

## 2. Compile with neutral paths and fixed time

Keep the variables from step 1. The exact published source is the preceding
source commit `11aa27458bc893646d41af18e95cc3110f438fe2`, with the `ThermalCam/`
tree recorded in the manifest. Later documentation/manifest commits do not
change that code. Changes to source, defaults or dependencies require a new
source commit, build and manifest; do not reuse this source attestation.

```sh
B="$STATE/build-continuation/build-a"
mkdir -p "$B"
FLAGS="-ffile-prefix-map=$REPO=/src/esp32-thermal-camera -fdebug-prefix-map=$REPO=/src/esp32-thermal-camera -ffile-prefix-map=$STATE=/toolchain -fdebug-prefix-map=$STATE=/toolchain -ffile-prefix-map=$B=/build/output -fdebug-prefix-map=$B=/build/output"
env -i PATH=/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin \
  HOME="$STATE/arduino-isolated/home" TMPDIR="$STATE/arduino-isolated/tmp" \
  LANG=C LC_ALL=C TZ=UTC SOURCE_DATE_EPOCH=1791051543 PYTHONDONTWRITEBYTECODE=1 \
  arduino-cli --config-file "$CFG" --no-color compile \
  --fqbn 'esp32:esp32:esp32s3:USBMode=hwcdc,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi' \
  --library "$STATE/deps/Arduino_GFX-2685a776495be1f9eaf8c572cf876469bcc56585" \
  --build-path "$B" --jobs 2 --verbose \
  --build-property "compiler.cpp.extra_flags=$FLAGS" \
  --build-property "compiler.c.extra_flags=$FLAGS" \
  --build-property "compiler.S.extra_flags=$FLAGS" \
  "$REPO/ThermalCam" > "$STATE/compile.log" 2>&1
shasum -a 256 "$B/ThermalCam.ino.elf" "$B/ThermalCam.ino.bin" \
  "$B/ThermalCam.ino.merged.bin"
cat "$B/flash_args"
cat "$B/partitions.csv"
```

Only `*.extra_flags` are overridden. Core 3.3.12 retains its platform
`compiler.c.flags` / `compiler.cpp.flags`, including **`-MMD -c`**, and all
SDK response-file flags. Prefix maps affect local source/debug paths; they do
not rewrite precompiled SDK debug information. **Keep ELF, map, build-options,
logs and caches private**; an ELF can contain upstream SDK-builder paths even
when the flashed binary does not.

The core automatically produces a **16,777,216-byte merged image**, padded
with `0xFF`, from these build outputs (also recorded in `flash_args`):

| Offset | Input |
| --- | --- |
| `0x0` | `ThermalCam.ino.bootloader.bin` |
| `0x8000` | `ThermalCam.ino.partitions.bin` |
| `0xe000` | SDK `boot_app0.bin` (OTA initialization, not device data) |
| `0x10000` | `ThermalCam.ino.bin` (newly linked application) |

Do not replace any input with a board dump. The published merged image has
blank `0xFF` NVS, second application slot, FAT partition and coredump partition.
Its OTA-data partition deliberately contains the SDK's `boot_app0.bin`.

The inherited IDF application description reports `arduino-lib-builder`,
version `6671d0b`, and the SDK's September 16, 2026 build date. Those fields are
**not** the thermal source commit or this compile's date. esptool inserts the
actual application ELF SHA-256; that matches the ELF hash in the manifest.

## 3. Release gate

1. Commit intended source first; record its commit and code-tree hash. Check
   every source-file hash against that commit; exclude the future manifest
   commit from the source attestation to avoid a circular self-reference.
2. Confirm a full compile/link and image-generation success, not only objects
   or a bootloader. Verify each merged range against its actual input hash,
   partition-table MD5, image checksums/digests, flash layout and blank NVS.
3. Run offline tests/privacy checker and independent binary raw, printable,
   UTF-16, container and entropy review. Heuristic passes are not proof of
   absence; resolve candidate strings before publication. Never publish logs,
   captured scenes, private paths, board dumps, IDs or credentials.
4. Retain required licenses and corresponding source/rebuild access; see
   `THIRD_PARTY.md` and `LICENSES/upstream-manifest.json`. Do not relicense the
   SDK binary as MIT. Modified-library relinking/rebuilding is permitted by
   the applicable licenses; these instructions impose no extra restriction.
5. Repeat in a separate **new** build directory/cache with the same epoch and
   normalized paths. Compare the actual ELF, app and merged bytes. Two local
   builds were byte-identical for this artifact; other hosts/toolchains and
   future downloads are not certified bitwise reproducible.
6. Preserve the prior binary privately before replacing the tracked candidate.
   Update manifest and tests together; run `git diff --check` and a fresh audit.
   Publication/settings/push and any hardware test require their own approval.

No board was accessed, flashed or tested for the October 5 build. Historical
firmware/source correspondence remains unproven; replacing the current file
neither validates nor removes historical bytes or previously distributed copies.
