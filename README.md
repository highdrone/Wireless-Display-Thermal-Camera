# ESP32-S3 AMOLED Thermal Camera

Live thermal video from a **Waveshare MLX90640 thermal camera module** ("MLX9064X Thermal Camera" board) on a **Waveshare ESP32-S3-Touch-AMOLED-1.8** (368×448 AMOLED).

![Screen preview](preview.png)

*Simulated preview with a fake scene (a person, a hot mug, a cold window). It was made by running this sketch's drawing code on a PC.*

- The 32×24 sensor image is smoothed and upscaled to fill the screen (448×336, landscape).
- Top left: temperature at the crosshair (the center of the image).
- The ring marks the hottest spot.
- Top right: battery level, with a lightning bolt while charging. It shows "USB" when running without a battery.
- The bar at the bottom shows the coldest and hottest temperatures and the color scale. The scale adjusts itself to the scene.
- The screen flips to stay right side up whichever way you hold the board in landscape. It uses the board's built-in motion sensor.
- After a minute without use, a 10-second countdown appears. Tap the screen to keep going. Otherwise the camera turns off.
- About 8 frames per second.
- Both board revisions work: the original (SH8601 display) and V2 (CO5300 display). The sketch detects which one you have.

## Controls

| Button | Action |
| --- | --- |
| BOOT, short press | Next palette: Ironbow → Rainbow → Hot → White hot → Black hot |
| BOOT, hold (~1 s) | Switch between °C and °F |
| PWR, short press | Save a picture to the microSD card. When off, it turns the camera on. |
| PWR, long press | Turn the board off (handled by the power chip itself) |
| Tap the screen | Keep the camera on during the auto-off countdown, or wake a dark screen |

The board remembers your palette and unit choice after power-off.

## Auto-off

The camera counts as idle when nobody presses a button, taps the screen, turns the board over, or plugs in or unplugs USB. After 50 idle seconds a countdown appears, and at 60 seconds:

- **On battery:** the camera powers off completely. Press **PWR** to turn it back on.
- **On USB power:** only the screen turns off. Tap it, or press any button, to wake it. Waking presses don't change the palette or save a picture.

Change the times with `IDLE_OFF_SECONDS` and `IDLE_WARNING_SECONDS` in `config.h`, or set `IDLE_OFF_SECONDS` to 0 to turn auto-off off.

## Saving pictures

Put a FAT32-formatted microSD card in the slot. Each short press of **PWR** saves two files into a `thermal` folder on the card:

- `IMG_0001.bmp`: the screen exactly as shown, with the readings and color scale (448×368 pixels).
- `IMG_0001.csv`: the 32×24 temperatures in °C, laid out like the picture. It opens in Excel or Numbers.

The top-right corner briefly shows **Saved IMG_0001**. If no card is found, it shows **No SD card**. Numbering continues from the last picture, even after power-off.

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

**Mounting:** put the sensor on the back of the board, pointing away from you. If the picture moves the wrong way when you pan, change `MIRROR_IMAGE` in `config.h`. The screen flips itself to stay right side up. By default the picture flips with it, which suits a sensor on loose wires. Once the sensor is fixed to the board, set `SENSOR_FIXED_TO_BOARD true` so only the text flips. If auto-rotate turns the screen upside down instead of right side up, set `AUTO_ROTATE_INVERT true`.

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
| `SCREEN_ROTATION` | 1 | Starting orientation: 1 or 3 = landscape, 0 or 2 = portrait (portrait only without auto-rotate) |
| `AUTO_ROTATE` | true | Flip between the two landscape orientations to match how the board is held |
| `AUTO_ROTATE_INVERT` | false | Set true if auto-rotate turns the screen upside down |
| `SENSOR_FIXED_TO_BOARD` | false | Set true once the sensor is mounted on the board: the picture then stays put and only the text flips |
| `MIRROR_IMAGE` | true | Flip the picture left/right. True is correct with the sensor pointing away from you. |
| `FLIP_IMAGE` | false | Turn the picture upside down |
| `SCREEN_BRIGHTNESS` | 200 | 0-255 |
| `SCREEN_CORNER_RADIUS` | 48 | Size of the screen's rounded corners in pixels. Text near the corners moves inward to clear them. |
| `START_IN_FAHRENHEIT` | false | Starting unit (the BOOT button overrides it) |
| `IDLE_OFF_SECONDS` | 60 | Turn off after this many idle seconds (0 = never) |
| `IDLE_WARNING_SECONDS` | 10 | Length of the "tap screen to keep using" countdown |
| `MIN_SPAN_C` | 3.0 | Smallest temperature range the colors stretch over. Stops noise from looking like detail. |

## Troubleshooting

- **"No SD card" when saving:** the card must be microSD, formatted FAT32 (cards of 32 GB or less come that way). Push it in until it clicks.
- **Corner text still clipped:** raise `SCREEN_CORNER_RADIUS` in `config.h`.
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
- Once running, the sensor task on core 0 is the only code that uses the board's I2C bus. Between frames it also reads the power chip (PWR button and battery), the touch chip and the motion sensor. Core 1 reads the results, so no two tasks ever talk on the bus at the same time.
