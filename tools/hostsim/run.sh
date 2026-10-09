#!/bin/sh
# Builds and runs the host simulation tests: the real ThermalCam.cpp and
# ThermalCamHead.cpp compiled for this computer against small stand-ins for
# the ESP32, display, sensor, color camera, radio and SD card. This is not an
# ESP32 build or a hardware test.
#
# Usage: tools/hostsim/run.sh [path/to/Arduino_GFX checkout]
# Without a path, the pinned Arduino_GFX revision is fetched with git.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd -P)"
REPO="$(cd "$HERE/../.." && pwd -P)"
GFX_REV=2685a776495be1f9eaf8c572cf876469bcc56585
WORK="$(mktemp -d "${TMPDIR:-/tmp}/thermal-hostsim.XXXXXX")"
if [ $# -ge 1 ]; then
  GFX="$(cd "$1" && pwd -P)"
else
  GFX="$WORK/Arduino_GFX"
  git init -q "$GFX"
  git -C "$GFX" fetch -q --depth 1 https://github.com/moononournation/Arduino_GFX "$GFX_REV"
  git -C "$GFX" checkout -q FETCH_HEAD
fi
CXX="${CXX:-c++}"
status=0
# head runs before fusion: fusion plays the stream that head records.
for t in markers_viewer link screen_timeout screen_standby camera_standby head fusion; do
  mkdir -p "$WORK/$t"
  if [ "$t" = head ]; then
    "$CXX" -std=gnu++17 -O1 -w -I "$HERE/shim" -I "$REPO/ThermalCamHead" -x c++ "$HERE/${t}_test.cpp" -o "$WORK/$t/sim"
  else
    "$CXX" -std=gnu++17 -O1 -w -I "$HERE/shim" -I "$GFX/src" -I "$REPO/ThermalCam" -x c++ "$HERE/${t}_test.cpp" \
      "$GFX/src/Arduino_GFX.cpp" "$GFX/src/Arduino_G.cpp" "$GFX/src/Arduino_DataBus.cpp" \
      "$GFX/src/canvas/Arduino_Canvas.cpp" -o "$WORK/$t/sim"
  fi
  echo "== $t"
  (cd "$WORK/$t" && ./sim "$HERE/data" "$WORK/head/head_stream.bin" > out.txt 2>&1) || status=1
  grep -v '^saved ' "$WORK/$t/out.txt" || true
  if grep -q '(bad)' "$WORK/$t/out.txt"; then status=1; fi
done
echo "Screenshots (.ppm) and the simulated SD card are in $WORK"
exit $status
