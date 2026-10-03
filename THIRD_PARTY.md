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

## Current compiled image and retained notices

The current merged image is newly built from the exact source/dependency set in
`firmware/manifest.json`; it replaces the source-unproven legacy image in the
current tree only. `firmware/BUILDING.md` provides the isolated install,
source build and release gate. The original project source and all vendored
source are available here; the pinned framework/library/SDK source is available
from the exact upstream revisions below. Rebuild/relink with modified libraries
using those sources and the recorded compiler options; the project imposes no
restriction on debugging such modifications or reverse engineering permitted
by the applicable licenses.

`LICENSES/upstream-manifest.json` records exact hashes and origin URLs/archive
members for retained, unmodified upstream notice texts. These include Arduino
ESP32 LGPL-2.1, ESP-IDF Apache-2.0, FreeRTOS MIT, lwIP BSD, WPA-supplicant BSD,
FatFs, Newlib notices, Tinycrypt BSD and Protobuf-C BSD. MbedTLS offers
Apache-2.0 OR GPL-2.0-or-later; this distribution uses its Apache-2.0 option,
while retaining its complete upstream license text. GCC runtime libraries have
the GCC Runtime Library Exception 3.1 with GPLv3; both texts are retained.
Public copyright contacts inside mandatory notices are retained verbatim,
not project-owner private contacts. The privacy checker recognizes only the
exact reviewed hashes of three such notices, never an entire license directory.

Relevant source pins:

- Arduino ESP32 **3.3.12**:
  `94afccf35fb1e401facddbcf9e13bcf7c76a31d8`.
- ESP-IDF **5.5.5**:
  `b774170ff46c393eeb5e495ea37936038d3f4f4f`.
  Its submodule revisions for lwIP, MbedTLS, Protobuf-C, Espressif Wi-Fi and
  PHY are encoded in the notice source URLs. Retained archive hashes identify
  the actual precompiled SDK/runtime bytes; an IDF source pin alone does not
  imply those libraries were rebuilt here.
- Arduino_GFX and Melexis source pins are listed above and in the build manifest.
- Xtensa compiler/runtime **esp-14.2.0_20260121 / GCC 14.2.0**:
  the official Espressif tool archive and its SHA-256 are in the manifest.

Before distributing any new binary, retain all applicable notices, review the
exact linked components and fulfill their source/relinking obligations. The
retained notices and pins are **not an exhaustive linked-component bill of
materials or a complete binary license-compliance certification**. The SDK's
package component inventory also includes libraries that are not necessarily
linked. Historical binary source/dependency mapping remains unverified; no
legacy rebuild or history rewrite was performed.

Upstream references:

- https://github.com/melexis/mlx90640-library
- https://github.com/moononournation/Arduino_GFX
- https://github.com/espressif/arduino-esp32
