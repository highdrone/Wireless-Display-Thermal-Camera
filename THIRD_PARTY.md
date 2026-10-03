# Third-party notices and firmware redistribution

The root MIT license covers original project code; it does **not** relicense
third-party components or every byte of a compiled firmware image.

- **Melexis MLX90640 library**, Melexis N.V., revision
  `f6be7ca1d4a55146b705f3d347f84b773b29cc86`: Apache-2.0.
  Its license and original copyright headers remain in
  `ThermalCam/src/mlx90640/`. Local include changes are marked in the modified
  file. The Arduino Wire adapter is documented in that directory's README.
- **GFX Library for Arduino 1.6.8**, upstream revision
  `2685a776495be1f9eaf8c572cf876469bcc56585`: preserve its source notices and
  bundled BSD-style license, including the Adafruit Industries copyright.
  The exact upstream `license.txt` is reproduced in `LICENSES/Arduino_GFX-BSD.txt`.
  This dependency is downloaded, not vendored as source here.
- **Arduino core for ESP32**: Arduino framework files include LGPL-2.1-or-later
  notices; the SDK and other bundled components have their own licenses. Follow
  the exact version's notices and redistribution requirements, including source
  and relinking obligations where applicable. Do not label the whole firmware
  as solely MIT.
- **Waveshare** examples/schematics were a hardware reference, as credited in
  README. This project is not affiliated with Waveshare, Melexis, Arduino or
  Adafruit.

Before distributing a new binary, retain all applicable dependency notices,
review the exact linked components and fulfill their license obligations.
The retained legacy binary's source and dependency mapping are unverified;
these notices are not a complete binary bill of materials or compliance
certification. Rebuild from documented source rather than relying on an
unverifiable artifact.

Upstream references:

- https://github.com/melexis/mlx90640-library
- https://github.com/moononournation/Arduino_GFX
- https://github.com/espressif/arduino-esp32
