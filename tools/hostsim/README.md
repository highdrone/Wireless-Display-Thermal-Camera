# Host simulation tests

These compile the real `ThermalCam/ThermalCam.cpp` and
`ThermalCamHead/ThermalCamHead.cpp` for your computer, with small stand-ins
(`shim/`) for the ESP32 core, display bus, MLX90640 driver, color camera, JPEG
decoder, ESP-NOW radio, SD card, deep sleep and power chip. They exercise logic
and drawing only. **They are not an ESP32 build and not a hardware test.**

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
| `head_test.cpp` | The HT-HC33 camera head: finding the sensor on either pin pair (either way round), problem messages, thermal frames, color pictures only when asked, standby and the USER key. Records its stream for `fusion_test` |
| `fusion_test.cpp` | A wireless screen playing the head's stream: color pictures decoded, the four views, swipes, BOOT, alignment (hold, drag, zoom, flip, save), falling back to thermal when pictures stop, the head's problem page |

Screenshots are written as `.ppm` files (raw panel orientation; the fusion
ones as the viewer sees them) in the temporary folder the script prints.
`data/fusion_scene.jpg` is a drawn room matching the made-up thermal scene, and
`data/fusion_scene.rgb565` the same picture decoded; `make_fusion_scene.py`
(needs Pillow) redraws them.
