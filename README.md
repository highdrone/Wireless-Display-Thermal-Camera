# ESP32-S3 AMOLED Thermal Camera

Live thermal video from a **Waveshare MLX90640 thermal camera module** ("MLX9064X Thermal Camera" board) on a **Waveshare ESP32-S3-Touch-AMOLED-1.8** (368×448 AMOLED).

![Screen preview](preview.png)

*Simulated preview with a fake scene (a person, a hot mug, a cold window). It was made by running this sketch's drawing code on a PC.*

- The 32×24 sensor image is smoothed and upscaled to fill the screen (448×336, landscape).
- Top left: temperature at the crosshair (the center of the image).
- The ring marks the hottest spot.
- The bar at the bottom shows the coldest and hottest temperatures and the color scale. The scale adjusts itself to the scene.
- About 8 frames per second.
- Both board revisions work: the original (SH8601 display) and V2 (CO5300 display). The sketch detects which one you have.

## Controls

| BOOT button | Action |
| --- | --- |
| Short press | Next palette: Ironbow → Rainbow → Hot → White hot → Black hot |
| Hold (~1 s) | Switch between °C and °F |

The board remembers your palette and unit choice after power-off.

## 1. Check which sensor you have

This code is for the **MLX90640** (32×24 pixels). Waveshare sells the same blue board with an **MLX90641** (16×12 pixels), and that sensor needs a different driver. Your product listing or bag should say "MLX90640-D55" / "MLX90640-D110" (D55 = 55° field of view, D110 = 110°; both work with this code). You can also read the marking on the round metal sensor can. If yours says MLX90641, the code needs changes.

## 2. Wiring

The sensor has 4 wires. Trace each one back to its label on the sensor board (SCL / SDA / GND / VCC). Don't go by wire color.

| Sensor board | ESP32-S3-AMOLED-1.8 pad |
| --- | --- |
| VCC | **3V3** |
| GND | **GND** |
| SDA | **SDA** (I2C pad, GPIO15) |
| SCL | **SCL** (I2C pad, GPIO14) |

The AMOLED board's expansion pads are small (1.27 mm pitch) solder pads. Cut off the Dupont plugs and solder the wires on, or solder a short 1.27 mm header. Check the silkscreen next to the pads.

The I2C pads connect to the board's internal I2C bus, which the touch, power and clock chips also use. That's fine: the MLX90640 has its own address (0x33), and this is the setup the sketch expects by default.

**Using two GPIO pads instead of the I2C pads?** Put those GPIO numbers in `THERMAL_SDA` / `THERMAL_SCL` in `ThermalCam/config.h`. The sensor then gets its own I2C bus at 800 kHz.

**Mounting:** put the sensor on the back of the board, pointing away from you. If the picture moves the wrong way when you pan, set `MIRROR_IMAGE true` in `config.h`. If it's upside down, change `SCREEN_ROTATION` from 1 to 3.

## 3a. Quick flash (no Arduino needed, default settings)

`firmware/ThermalCam-full-flash-at-0x0.bin` is this sketch built with the default `config.h`.

1. Open <https://espressif.github.io/esptool-js/> in Chrome or Edge.
2. Plug in the board and click **Connect**. If no port shows up, unplug, then hold **BOOT** while plugging the board back in.
3. Set the flash address to **0x0**, choose the `.bin` file, and click **Program**.
4. Unplug and replug the board (or press its power button) to start it.

## 3b. Build it yourself (Arduino IDE 2.x)

1. **Board support:** go to *File → Preferences → Additional boards manager URLs* and add
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`.
   Then open *Boards Manager* and install **esp32 by Espressif Systems** (version 3.x).
2. **Libraries:** in *Library Manager*, install
   - **GFX Library for Arduino** (by Moon On Our Nation)
   - **Adafruit MLX90640**. When asked, also install its dependency **Adafruit BusIO**.
3. **Board settings** (*Tools* menu):
   | Setting | Value |
   | --- | --- |
   | Board | ESP32S3 Dev Module |
   | USB CDC On Boot | Enabled |
   | Flash Size | 16MB (128Mb) |
   | Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
   | **PSRAM** | **OPI PSRAM** ← required. The screen buffer lives in PSRAM. |
4. Open `ThermalCam/ThermalCam.ino` and click **Upload**.
5. Optional: open the Serial Monitor at 115200 baud. Every 5 seconds it prints the frame rate and the temperatures.

Tested build: esp32 core 3.3.12, GFX Library for Arduino 1.6.8, Adafruit MLX90640 1.1.2.

## Settings (`ThermalCam/config.h`)

| Setting | Default | What it does |
| --- | --- | --- |
| `THERMAL_SDA` / `THERMAL_SCL` | 15 / 14 | Sensor I2C pins |
| `SENSOR_REFRESH` | `MLX90640_16_HZ` | 16 Hz ≈ 8 frames/s. 32 Hz is faster but noisier, and only keeps up on a dedicated bus. |
| `SCREEN_ROTATION` | 1 | 1 or 3 = landscape, 0 or 2 = portrait |
| `MIRROR_IMAGE` / `FLIP_IMAGE` | false | Flip the picture left/right or up/down |
| `SCREEN_BRIGHTNESS` | 200 | 0-255 |
| `START_IN_FAHRENHEIT` | false | Starting unit (the BOOT button overrides it) |
| `MIN_SPAN_C` | 3.0 | Smallest temperature range the colors stretch over. Stops noise from looking like detail. |

## Troubleshooting

- **"Sensor not found" on screen:** the screen lists every I2C address it can see.
  - Other devices listed but no `0x33`: the bus works, but the sensor isn't connected. Check VCC/GND and that SDA/SCL aren't swapped.
  - `none`: the pins in `config.h` don't match where you soldered.
- **"PSRAM is off" on screen:** set *Tools → PSRAM → OPI PSRAM* and upload again.
- **Black screen, nothing at all:** unplug the board and plug it back in without holding BOOT; it won't run the new firmware while it's still in flashing mode. If it's still black, read the board's status messages. In the Arduino IDE, open the Serial Monitor at 115200 baud. On the esptool-js page, use the **Console** section: click **Connect**, pick the port, then click **Reset**. When no sensor is connected, a status line repeats every 2 seconds and names the display revision and PSRAM size.
- **"Sensor stopped" after it was working:** loose wire, or a long or noisy cable. Keep the sensor wires short.
- **Checkerboard pattern on moving objects:** the shared bus isn't keeping up with the sensor. Set `SENSOR_REFRESH` to `MLX90640_8_HZ`.
- **Readings seem low on shiny metal:** this is normal for all thermal cameras. Shiny surfaces reflect heat instead of giving it off. Readings assume emissivity 0.95, which is right for skin, wood, paint, plastic and food. Put a piece of matte tape on metal to measure it.
- The sensor reads about ±1-2 °C absolute. It settles after a few minutes of warm-up.

## How it works

- The sensor is read on CPU core 0 at up to 16 sub-pages/s, using the Adafruit/Melexis calibration math. Finished frames are passed to core 1.
- Core 1 upscales each 32×24 frame with integer bilinear interpolation into a full-screen RGB565 frame buffer in PSRAM. It then draws the overlays and sends the whole buffer to the AMOLED over QSPI.
- Before the display starts, the sketch pulses the reset lines on the board's IO expander, the same way Waveshare's examples do. Both panels are started with the CO5300 init sequence, as Waveshare's own board driver does. The V2 panel also needs a 16-column offset. The sketch applies it unless the original board's FT3168 touch chip answers.
