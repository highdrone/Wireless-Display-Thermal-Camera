# Host simulation tests

These compile the real `ThermalCam/ThermalCam.cpp` for your computer, with
small stand-ins (`shim/`) for the ESP32 core, display bus, MLX90640 driver,
ESP-NOW radio, SD card, deep sleep and power chip. They exercise logic and
drawing only. **They are not an ESP32 build and not a hardware test.**

```sh
tools/hostsim/run.sh                 # fetches Arduino_GFX 2685a776 with git
tools/hostsim/run.sh path/to/Arduino_GFX
```

Needs a C++17 compiler and git. Each test prints what it measured; a line
ending in `(bad)` or a non-zero exit means a failure.

| Test | What it checks |
| --- | --- |
| `markers_viewer_test.cpp` | Smoothing steadiness, hot/cold marker stability and tracking, saving BMP/CSV, the picture viewer and touch coordinates |
| `link_test.cpp` | Camera → wireless screen frames: packet sizes, values arriving intact, staying with one camera |
| `screen_timeout_test.cpp` | The wireless screen's "Lost the camera's signal" countdown and turn-off |
| `screen_standby_test.cpp` | Screen standby: probe hellos, wake-ups (camera on, tap/BOOT), the 30-minute cut-off |
| `camera_standby_test.cpp` | Camera standby: no auto-off while a screen watches, standby after it leaves, waking only for a switched-on screen, USB behaviour |

Screenshots are written as `.ppm` files (raw panel orientation) in the
temporary folder the script prints.
