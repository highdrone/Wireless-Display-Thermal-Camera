// Thermal camera: Waveshare MLX90640 thermal sensor (32x24 pixels) shown on a
// Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448 AMOLED). Works on both board
// revisions (SH8601 + FT3168, and V2 with CO5300 + CST820).
//
// Camera: BOOT short press = next color palette, BOOT hold = switch C / F,
// PWR short press = save a picture (BMP) and temperatures (CSV) to microSD.
// Swipe left to right for the picture viewer, right to left to come back.
// Viewer: tap the right/left half (or press BOOT) to step through pictures.
// After a minute without use the camera turns off (tap to keep it on).
//
// Wireless screen: flash this same firmware to a second board with no thermal
// sensor. It finds no sensor, becomes a wireless screen, and shows the
// camera's picture over ESP-NOW (direct radio, no router).
//
// Library (Arduino Library Manager): "GFX Library for Arduino" by Moon On Our
// Nation. The MLX90640 driver is Melexis' own, included in src/mlx90640.
// Board settings: see README.md (PSRAM must be set to "OPI PSRAM").

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <algorithm>
#include <vector>

#include "config.h"
#include "palettes.h"
#include "src/mlx90640/mlx90640.h"

// ---- Board pins (same on both revisions) -----------------------------------
#define LCD_CS 12
#define LCD_SCLK 11
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_WIDTH 368
#define LCD_HEIGHT 448
#define BOARD_SDA 15
#define BOARD_SCL 14
#define BOOT_BUTTON 0
#define SD_CLK 2
#define SD_CMD 1
#define SD_D0 3
#define TP_INT 21  // touch interrupt

#define MLX_ADDR 0x33
#define TCA9554_ADDR 0x20  // IO expander; EXIO0-2 are display/touch reset lines
#define FT3168_ADDR 0x38   // touch chip on the original board (SH8601 panel)
#define CST820_ADDR 0x15   // touch chip on the V2 board
#define AXP2101_ADDR 0x34  // power chip; the PWR button is wired to it
#define AXP2101_INTEN2 0x41   // IRQ enable 2
#define AXP2101_INTSTS2 0x49  // IRQ status 2, write 1 to clear
#define AXP2101_PKEY_SHORT 0x08  // bit 3 of both: PWR button short press
#define AXP2101_STATUS1 0x00      // bit 5: USB power present, bit 3: battery present
#define AXP2101_STATUS2 0x01      // bits 6-5: 1 = charging
#define AXP2101_COMMON_CONFIG 0x10  // bit 0: power off
#define AXP2101_GAUGE_CTRL 0x18   // bit 3: fuel gauge on
#define AXP2101_BAT_DET_CTRL 0x68 // bit 0: battery detection on
#define AXP2101_BAT_PERCENT 0xA4

#define SENSOR_W 32
#define SENSOR_H 24
#define SENSOR_PIXELS (SENSOR_W * SENSOR_H)
#define EMISSIVITY 0.95f
#define TA_SHIFT 8.0f              // reflected temperature = sensor temperature - 8 C (open air)
#define BAR_H 32                   // height of the scale bar under the picture
#define SCREEN_ROT (SCREEN_ROTATION & 3)
#define DEDICATED_BUS_HZ 800000    // ESP32-S3 I2C tops out around 800 kHz
#define EDGE_PAD 4                 // gap between text and the screen's curved edge
#define MARKER_HYSTERESIS 0.2f     // C a spot must beat the marked one by to take the marker
#define SWIPE_MIN_PX 60
#define TAP_MAX_PX 30
#define WARNING_BELOW_TEXT 236     // countdown position on text pages, under the text

static const bool SENSOR_ON_BOARD_BUS = (THERMAL_SDA == BOARD_SDA && THERMAL_SCL == BOARD_SCL);

static const uint16_t COLOR_BLACK = 0x0000;
static const uint16_t COLOR_WHITE = 0xFFFF;
static const uint16_t COLOR_GREEN = 0x07E0;
static const uint16_t COLOR_YELLOW = 0xFFE0;
static const uint16_t COLOR_RED = 0xF800;
static const uint16_t COLOR_HOT = 0xFA45;   // (255, 72, 40)
static const uint16_t COLOR_COLD = 0x565F;  // (80, 200, 255)

enum Gesture : uint8_t { GESTURE_NONE, GESTURE_TAP, GESTURE_SWIPE_RIGHT, GESTURE_SWIPE_LEFT, GESTURE_OTHER };
enum Mode : uint8_t { MODE_CAMERA, MODE_VIEWER };

static Arduino_DataBus *bus;
static Arduino_OLED *panel;
static Arduino_Canvas *gfx;  // full-screen frame buffer in PSRAM, pushed with flush()
static Preferences prefs;
static paramsMLX90640 mlxParams;

// Frames are read on core 0 and handed to the display loop on core 1.
static float sharedFrame[SENSOR_PIXELS];
static bool sharedFrameNew = false;
static portMUX_TYPE frameLock = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t sensorErrorCount = 0;
static volatile int lastSensorError = 0;

// Layout, in rotated screen coordinates.
static int16_t scrW, scrH, imgX, imgY, imgW, imgH, imgScale;
// Bilinear upscaling tables: output pixel i blends sensor cells src0[i] and
// src1[i], giving src1 a weight of weight[i] / 256.
static uint8_t xSrc0[LCD_HEIGHT], xSrc1[LCD_HEIGHT], xWeight[LCD_HEIGHT];
static uint8_t ySrc0[LCD_HEIGHT], ySrc1[LCD_HEIGHT], yWeight[LCD_HEIGHT];
// Frame buffer index of rotated pixel (x, y) is fbBase + x * fbDx + y * fbDy.
static int32_t fbBase, fbDx, fbDy;

static uint16_t paletteLut[PALETTE_COUNT][256];
static uint8_t paletteIdx = 0;
static bool fahrenheit = START_IN_FAHRENHEIT;
static const char *boardName = "?";
static char toastText[24] = "";
static uint32_t toastUntil = 0;
static bool screenDirty = true;  // a message screen left text outside the picture
static bool boardIsV1 = false;
static bool pmuFound = false;
static bool sdMounted = false;
static uint8_t touchAddr = 0;
static Mode mode = MODE_CAMERA;
static bool isCamera = true;     // false: no sensor, so this board is a wireless screen
static bool waitingShown = false;  // screen: the "waiting for the camera" page is up
static String sensorlessNote;    // why this board is a screen (I2C scan)

// Written by the sensor task, which is the only user of the board's I2C bus
// once it runs, and by the touch interrupt.
static TaskHandle_t boardTaskHandle = nullptr;
static volatile bool powerKeyPressed = false;
static volatile bool touchSeen = false;
static volatile uint8_t gesture = GESTURE_NONE;  // finished touch, with where and when it started
static volatile int16_t gestureX = 0;
static volatile uint32_t gestureStartMs = 0;
static volatile bool battKnown = false, battPresent = false, battCharging = false, vbusPresent = false;
static volatile int8_t battPercent = -1;
static volatile bool powerOffRequested = false;

// Smoothed picture and readouts.
struct FrameStats {
  float minT, maxT, centerT;
  int minIdx, maxIdx;
};
static float smoothT[SENSOR_PIXELS];
static bool haveFrame = false;
static FrameStats shown = {NAN, NAN, NAN, -1, -1};  // readouts on screen; minIdx/maxIdx = markers
static bool markersVisible = false;
static float rangeLo = NAN, rangeHi = NAN;  // color scale, degrees C

// Wireless screen link (ESP-NOW broadcast). The screen says hello twice a
// second; the camera streams only while it hears one.
#define LINK_MAGIC 0x54  // 'T'
#define LINK_VERSION 1
#define LINK_CHUNK_PIXELS 112
#define LINK_CHUNKS ((SENSOR_PIXELS + LINK_CHUNK_PIXELS - 1) / LINK_CHUNK_PIXELS)
enum : uint8_t { PKT_HELLO = 1, PKT_META = 2, PKT_PIXELS = 3 };
struct __attribute__((packed)) PktHeader {
  uint8_t magic, version, type;
  uint16_t frame;
};
struct __attribute__((packed)) PktMeta {  // what the camera shows besides the pixels
  PktHeader h;
  uint8_t palette, flags;  // flags: bit 0 = Fahrenheit, bit 1 = markers visible
  int16_t hotIdx, coldIdx;
  float centerT, minT, maxT, rangeLo, rangeHi;
};
struct __attribute__((packed)) PktPixels {  // temperatures in 1/100 C, INT16_MIN = no reading
  PktHeader h;
  uint8_t chunk, count;
  int16_t centi[LINK_CHUNK_PIXELS];
};
static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static bool linkReady = false;
static volatile uint32_t lastHelloMs = 0;  // camera: when a screen last said hello
// Screen: the newest frame from the camera, filled in by the receive callback.
static portMUX_TYPE linkLock = portMUX_INITIALIZER_UNLOCKED;
static PktMeta linkMeta;
static int16_t linkPixels[SENSOR_PIXELS];
static bool linkFrameReady = false;
static uint8_t linkCameraMac[6];
static volatile uint32_t linkCameraHeardMs = 0;
static uint32_t lastLinkFrameMs = 0;  // screen: when the last picture arrived
static bool everConnected = false;    // screen: has had a picture since startup

// Picture viewer.
static std::vector<uint16_t> pictures;  // numbers of the IMG_####.bmp files, oldest first
static int viewIdx = 0;
static uint16_t *viewerImage = nullptr;  // decoded picture, laid out like the frame buffer
static bool viewerHasImage = false;
static bool viewerDirty = false;
static const char *viewerNote = "";

// Auto-off.
static uint32_t lastActivityMs = 0;
static uint32_t idleWarnLeftMs = 0;  // > 0 while the countdown is showing
static bool screenAsleep = false;

// ============================================================================
//  I2C helpers
// ============================================================================

static bool i2cPresent(TwoWire &w, uint8_t addr) {
  w.beginTransmission(addr);
  return w.endTransmission() == 0;
}

static bool i2cReadRegs(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, len) != len) return false;
  for (uint8_t i = 0; i < len; i++) buf[i] = Wire.read();
  return true;
}

static bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t &val) { return i2cReadRegs(addr, reg, &val, 1); }

static void i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
}

static void i2cSetBits(uint8_t addr, uint8_t reg, uint8_t bits) {
  uint8_t v;
  if (i2cReadReg(addr, reg, v) && (v & bits) != bits) i2cWriteReg(addr, reg, v | bits);
}

static String scanBus(TwoWire &w) {
  String found;
  for (uint8_t a = 1; a < 127; a++) {
    if (i2cPresent(w, a)) {
      char buf[6];
      snprintf(buf, sizeof(buf), "0x%02X ", a);
      found += buf;
    }
  }
  return found.length() ? found : String("none");
}

// ============================================================================
//  Board chips: IO expander, power chip, touch
// ============================================================================

// Pulse EXIO0-2 low then high: the reset sequence Waveshare's examples use.
static void resetDisplayAndTouch() {
  uint8_t out, cfg;
  if (!i2cReadReg(TCA9554_ADDR, 0x01, out) || !i2cReadReg(TCA9554_ADDR, 0x03, cfg)) {
    Serial.println("IO expander not found, skipping reset");
    return;
  }
  i2cWriteReg(TCA9554_ADDR, 0x01, out & ~0x07);  // output latch low
  i2cWriteReg(TCA9554_ADDR, 0x03, cfg & ~0x07);  // EXIO0-2 as outputs
  delay(20);
  i2cWriteReg(TCA9554_ADDR, 0x01, out | 0x07);
  delay(100);
}

// Power chip: latch PWR button short presses so we can poll for them, and
// make sure battery detection and the fuel gauge are running.
static void initPower() {
  uint8_t en;
  if (!i2cReadReg(AXP2101_ADDR, AXP2101_INTEN2, en)) {
    Serial.println("Power chip not found: no PWR button capture or battery level");
    return;
  }
  i2cWriteReg(AXP2101_ADDR, AXP2101_INTSTS2, AXP2101_PKEY_SHORT);  // drop any old press
  i2cWriteReg(AXP2101_ADDR, AXP2101_INTEN2, en | AXP2101_PKEY_SHORT);
  i2cSetBits(AXP2101_ADDR, AXP2101_BAT_DET_CTRL, 0x01);
  i2cSetBits(AXP2101_ADDR, AXP2101_GAUGE_CTRL, 0x08);
  pmuFound = true;
}

static void pollPowerKey() {
  uint8_t st;
  if (!pmuFound || !i2cReadReg(AXP2101_ADDR, AXP2101_INTSTS2, st)) return;
  if (st & AXP2101_PKEY_SHORT) {
    i2cWriteReg(AXP2101_ADDR, AXP2101_INTSTS2, AXP2101_PKEY_SHORT);
    powerKeyPressed = true;
  }
}

static void pollBattery() {
  uint8_t st[2], pct;
  if (!pmuFound || !i2cReadRegs(AXP2101_ADDR, AXP2101_STATUS1, st, 2)) return;
  vbusPresent = st[0] & 0x20;
  battPresent = st[0] & 0x08;
  battCharging = ((st[1] >> 5) & 0x03) == 1;
  if (!battPresent) {
    battPercent = -1;
  } else if (i2cReadReg(AXP2101_ADDR, AXP2101_BAT_PERCENT, pct) && pct <= 100) {
    battPercent = pct;
  }
  battKnown = true;
}

// Cuts the board's power, like holding PWR. Only from the task that owns I2C.
static void powerOffNow() { i2cSetBits(AXP2101_ADDR, AXP2101_COMMON_CONFIG, 0x01); }

static void IRAM_ATTR onTouchInterrupt() { touchSeen = true; }

static void initTouch() {
  touchAddr = boardIsV1 ? FT3168_ADDR : CST820_ADDR;
  if (!boardIsV1) {
    i2cWriteReg(CST820_ADDR, 0xFE, 0x01);  // no auto-sleep, so it always answers polls
    i2cWriteReg(CST820_ADDR, 0xFA, 0x60);  // pulse the interrupt line on touches
  }
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(TP_INT, onTouchInterrupt, FALLING);
}

// The touch chips report panel (portrait) coordinates, the same as the
// display's own; convert them to the rotated screen's.
static void panelToScreen(int16_t px, int16_t py, int16_t &x, int16_t &y) {
  switch (SCREEN_ROT) {
    case 1: x = py; y = LCD_WIDTH - 1 - px; break;
    case 2: x = LCD_WIDTH - 1 - px; y = LCD_HEIGHT - 1 - py; break;
    case 3: x = LCD_HEIGHT - 1 - py; y = px; break;
    default: x = px; y = py; break;
  }
}

// Follows one finger from touch to release, then reports a tap or a swipe.
// Registers 0x02-0x06 (finger count, then X and Y) match on both chips.
static void pollTouch() {
  static bool down = false;
  static int16_t x0, y0, x1, y1;
  static uint32_t t0;
  uint8_t d[5];
  if (!touchAddr || !i2cReadRegs(touchAddr, 0x02, d, sizeof(d))) return;
  const uint8_t fingers = d[0] & 0x0F;
  if (fingers >= 1 && fingers <= 5) {
    int16_t x, y;
    panelToScreen(((d[1] & 0x0F) << 8) | d[2], ((d[3] & 0x0F) << 8) | d[4], x, y);
    if (!down) {
      down = true;
      x0 = x;
      y0 = y;
      t0 = millis();
    }
    x1 = x;
    y1 = y;
    touchSeen = true;
  } else if (down) {
    down = false;
    const int16_t dx = x1 - x0, dy = y1 - y0;
    uint8_t g = GESTURE_OTHER;
    if (abs(dx) >= SWIPE_MIN_PX && abs(dx) > 2 * abs(dy)) {
      g = dx > 0 ? GESTURE_SWIPE_RIGHT : GESTURE_SWIPE_LEFT;
    } else if (abs(dx) < TAP_MAX_PX && abs(dy) < TAP_MAX_PX && millis() - t0 < 800) {
      g = GESTURE_TAP;
    }
    gestureX = x0;
    gestureStartMs = t0;
    gesture = g;
  }
}

// ============================================================================
//  Display
// ============================================================================

static bool initDisplay() {
  Wire.begin(BOARD_SDA, BOARD_SCL, 400000);
  resetDisplayAndTouch();

  // The touch chip tells the two board revisions apart. The original board's
  // FT3168 answers once it has booted; the V2 board's CST820 may be asleep, so
  // anything else is treated as V2.
  bool v1 = false;
  for (int i = 0; i < 10 && !v1; i++) {
    v1 = i2cPresent(Wire, FT3168_ADDR);
    if (!v1) delay(50);
  }
  boardIsV1 = v1;
  boardName = v1 ? "original (SH8601)" : "V2 (CO5300)";
  Serial.printf("Display: %s\n", boardName);
  Serial.printf("PSRAM: %u bytes\n", (unsigned)ESP.getPsramSize());

  // Waveshare's own driver starts both panels with the CO5300 init sequence;
  // only the V2 panel's 16-column offset differs.
  bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
  panel = new Arduino_CO5300(bus, GFX_NOT_DEFINED, 0, LCD_WIDTH, LCD_HEIGHT, v1 ? 0 : 16, 0, 0, 0);
  if (!panel->begin()) {
    Serial.println("Display init failed");
    return false;
  }
  panel->fillScreen(COLOR_BLACK);
  panel->setBrightness(SCREEN_BRIGHTNESS);

  gfx = new Arduino_Canvas(LCD_WIDTH, LCD_HEIGHT, panel, 0, 0, SCREEN_ROT);
  if (!gfx->begin(GFX_SKIP_OUTPUT_BEGIN)) {
    Serial.println("No memory for the frame buffer. Set Tools > PSRAM > OPI PSRAM.");
    panel->setTextColor(COLOR_WHITE);
    panel->setTextSize(2);
    panel->setCursor(20, 180);
    panel->print("PSRAM is off.");
    panel->setCursor(20, 210);
    panel->print("Arduino IDE: Tools >");
    panel->setCursor(20, 240);
    panel->print("PSRAM > OPI PSRAM");
    return false;
  }
  gfx->fillScreen(COLOR_BLACK);
  return true;
}

static void buildAxis(int outLen, int srcLen, bool reverse, uint8_t *src0, uint8_t *src1, uint8_t *weight) {
  for (int i = 0; i < outLen; i++) {
    float s = (i + 0.5f) * srcLen / outLen - 0.5f;
    if (s < 0) s = 0;
    if (s > srcLen - 1) s = srcLen - 1;
    const int a = (int)s;
    const int b = (a + 1 < srcLen) ? a + 1 : a;
    weight[i] = (uint8_t)((s - a) * 256);
    src0[i] = reverse ? srcLen - 1 - a : a;
    src1[i] = reverse ? srcLen - 1 - b : b;
  }
}

static void setupLayout() {
  scrW = gfx->width();
  scrH = gfx->height();
  imgScale = min(scrW / SENSOR_W, (scrH - BAR_H) / SENSOR_H);
  imgW = SENSOR_W * imgScale;
  imgH = SENSOR_H * imgScale;
  imgX = (scrW - imgW) / 2;
  imgY = (scrH - BAR_H - imgH) / 2;

  buildAxis(imgW, SENSOR_W, MIRROR_IMAGE, xSrc0, xSrc1, xWeight);
  buildAxis(imgH, SENSOR_H, FLIP_IMAGE, ySrc0, ySrc1, yWeight);

  // Same mapping as Arduino_Canvas::writePixelPreclipped().
  switch (SCREEN_ROT) {
    case 1: fbBase = LCD_WIDTH - 1; fbDx = LCD_WIDTH; fbDy = -1; break;
    case 2: fbBase = LCD_WIDTH * LCD_HEIGHT - 1; fbDx = -1; fbDy = -LCD_WIDTH; break;
    case 3: fbBase = (LCD_HEIGHT - 1) * LCD_WIDTH; fbDx = -LCD_WIDTH; fbDy = 1; break;
    default: fbBase = 0; fbDx = 1; fbDy = LCD_WIDTH; break;
  }
}

// How far in from the left/right edge text must start to clear the screen's
// rounded corners, for text whose nearest row is fromEdge pixels from the top
// or bottom edge.
static int16_t cornerInset(int16_t fromEdge) {
  const float r = SCREEN_CORNER_RADIUS;
  if (fromEdge >= r) return 0;
  if (fromEdge < 0) fromEdge = 0;
  const float dy = r - fromEdge;
  return (int16_t)ceilf(r - sqrtf(r * r - dy * dy));
}

// Left margin for a text row spanning y .. y + h - 1.
static int16_t textMargin(int16_t y, int16_t h) {
  return cornerInset(min<int16_t>(y, scrH - (y + h))) + EDGE_PAD;
}

// Full-screen text, one line per '\n'. Doesn't push it to the display.
static void drawMessage(const char *title, const String &body) {
  gfx->fillScreen(COLOR_BLACK);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(max<int16_t>(16, textMargin(24, 24)), 24);
  gfx->print(title);
  gfx->setTextSize(2);
  int16_t y = 76;
  int start = 0;
  while (start <= (int)body.length()) {
    int end = body.indexOf('\n', start);
    if (end < 0) end = body.length();
    gfx->setCursor(max<int16_t>(16, textMargin(y, 16)), y);
    gfx->print(body.substring(start, end));
    y += 24;
    start = end + 1;
  }
  screenDirty = true;
}

static void showMessage(const char *title, const String &body) {
  drawMessage(title, body);
  gfx->flush();
}

static void showToast(const char *text) {
  strlcpy(toastText, text, sizeof(toastText));
  toastUntil = millis() + 1500;
}

// ============================================================================
//  Sensor
// ============================================================================

static uint8_t refreshCode(int hz) {
  return hz >= 32 ? 0x06 : hz >= 16 ? 0x05 : hz >= 8 ? 0x04 : hz >= 4 ? 0x03 : 0x02;
}

static bool initSensorChip() {
  static uint16_t eeprom[MLX90640_EEPROM_DUMP_NUM];
  if (MLX90640_DumpEE(MLX_ADDR, eeprom) != 0) return false;
  const int rc = MLX90640_ExtractParameters(eeprom, &mlxParams);
  if (rc == -MLX90640_EEPROM_DATA_ERROR) {
    Serial.println("MLX90640 calibration data invalid (is it an MLX90641?)");
    return false;
  }
  if (rc != 0) Serial.printf("MLX90640 calibration note %d (a few dead pixels)\n", rc);
  MLX90640_SetChessMode(MLX_ADDR);
  MLX90640_SetResolution(MLX_ADDR, 0x02);  // 18-bit ADC
  MLX90640_SetRefreshRate(MLX_ADDR, refreshCode(SENSOR_REFRESH_HZ));
  return true;
}

static void noteSensorError(int rc) {
  lastSensorError = rc;
  sensorErrorCount = sensorErrorCount + 1;
}

// Reads each sub-page as soon as the sensor has it (camera only), and polls
// the touch chip (about 100 times a second) and the power chip in between.
static void boardTask(void *) {
  static uint16_t raw[834];
  static float frame[SENSOR_PIXELS];  // each sub-page refreshes half the pixels (chess pattern)
  uint8_t subpagesSeen = 0;
  uint32_t keyPolledAt = 0, batteryPolledAt = 0;
  for (;;) {
    uint16_t status;
    if (!isCamera) {
      // no sensor to read
    } else if (MLX90640_I2CRead(MLX_ADDR, MLX90640_STATUS_REG, 1, &status) != 0) {
      noteSensorError(-MLX90640_I2C_NACK_ERROR);
      vTaskDelay(pdMS_TO_TICKS(50));
    } else if (MLX90640_GET_DATA_READY(status)) {
      const int subpage = MLX90640_GetFrameData(MLX_ADDR, raw);
      if (subpage < 0) {
        noteSensorError(subpage);
      } else {
        const float tr = MLX90640_GetTa(raw, &mlxParams) - TA_SHIFT;
        MLX90640_CalculateTo(raw, &mlxParams, EMISSIVITY, tr, frame);
        subpagesSeen |= 1 << subpage;
        if (subpagesSeen == 3) {
          portENTER_CRITICAL(&frameLock);
          memcpy(sharedFrame, frame, sizeof(frame));
          sharedFrameNew = true;
          portEXIT_CRITICAL(&frameLock);
        }
      }
    }

    pollTouch();
    const uint32_t now = millis();
    if (now - keyPolledAt >= 50) {
      keyPolledAt = now;
      pollPowerKey();
    }
    if (now - batteryPolledAt >= 2000) {
      batteryPolledAt = now;
      pollBattery();
    }
    if (powerOffRequested) powerOffNow();
    vTaskDelay(pdMS_TO_TICKS(8));
  }
}

static bool takeFrame(float *dst) {
  bool got = false;
  portENTER_CRITICAL(&frameLock);
  if (sharedFrameNew) {
    memcpy(dst, sharedFrame, sizeof(sharedFrame));
    sharedFrameNew = false;
    got = true;
  }
  portEXIT_CRITICAL(&frameLock);
  return got;
}

// Returns false when there is no sensor and the board should be a wireless
// screen instead. Without WIRELESS_SCREEN it keeps looking for the sensor.
static bool startSensor() {
  TwoWire &w = SENSOR_ON_BOARD_BUS ? Wire : Wire1;
  if (!SENSOR_ON_BOARD_BUS) w.begin(THERMAL_SDA, THERMAL_SCL, DEDICATED_BUS_HZ);
  MLX90640_SetWire(&w);

  showMessage("Thermal camera", "Starting sensor...");
  for (int attempt = 0; !initSensorChip(); attempt++) {
    const String seen = scanBus(w);
    if (WIRELESS_SCREEN && attempt >= 2) {
      sensorlessNote = String("No thermal sensor on this board.\nI2C: ") + seen;
      Serial.printf("No MLX90640 (I2C devices: %s). Working as a wireless screen.\n", seen.c_str());
      return false;
    }
    if (WIRELESS_SCREEN) {  // a sensor answers at once; retry briefly for loose wires
      delay(300);
      continue;
    }
    Serial.printf("Display %s, PSRAM %u. MLX90640 not found on SDA=%d SCL=%d. I2C devices: %s\n",
                  boardName, (unsigned)ESP.getPsramSize(), THERMAL_SDA, THERMAL_SCL, seen.c_str());
    char where[40];
    snprintf(where, sizeof(where), "on SDA=GPIO%d, SCL=GPIO%d.", THERMAL_SDA, THERMAL_SCL);
    showMessage("Sensor not found",
                String("Looking for MLX90640 (0x33)\n") + where + "\nI2C devices seen:\n" + seen +
                    "\n\nCheck the 4 wires:\nVCC->3V3   GND->GND\nSDA->SDA   SCL->SCL\n\nRetrying...");
    delay(2000);
  }
  uint16_t serial[3] = {0, 0, 0};
  MLX90640_I2CRead(MLX_ADDR, 0x2407, 3, serial);
  Serial.printf("MLX90640 found, serial %04X%04X%04X\n", serial[0], serial[1], serial[2]);
  return true;
}

// ============================================================================
//  Frame processing
// ============================================================================

static inline bool validTemp(float v) { return v > -50.0f && v < 350.0f; }  // false for NaN

// Replace dead or glitched pixels with the average of their neighbors.
static void repairPixels(float *t) {
  for (int i = 0; i < SENSOR_PIXELS; i++) {
    if (validTemp(t[i])) continue;
    const int r = i / SENSOR_W, c = i % SENSOR_W;
    float sum = 0;
    int n = 0;
    if (c > 0 && validTemp(t[i - 1])) { sum += t[i - 1]; n++; }
    if (c < SENSOR_W - 1 && validTemp(t[i + 1])) { sum += t[i + 1]; n++; }
    if (r > 0 && validTemp(t[i - SENSOR_W])) { sum += t[i - SENSOR_W]; n++; }
    if (r < SENSOR_H - 1 && validTemp(t[i + SENSOR_W])) { sum += t[i + SENSOR_W]; n++; }
    t[i] = n ? sum / n : NAN;
  }
}

static FrameStats computeStats(const float *t) {
  FrameStats s = {INFINITY, -INFINITY, NAN, 0, 0};
  for (int i = 0; i < SENSOR_PIXELS; i++) {
    const float v = t[i];
    if (!validTemp(v)) continue;
    if (v < s.minT) { s.minT = v; s.minIdx = i; }
    if (v > s.maxT) { s.maxT = v; s.maxIdx = i; }
  }
  // The center reading averages the 4 pixels in the middle of the sensor.
  float sum = 0;
  int n = 0;
  for (int r = SENSOR_H / 2 - 1; r <= SENSOR_H / 2; r++) {
    for (int c = SENSOR_W / 2 - 1; c <= SENSOR_W / 2; c++) {
      const float v = t[r * SENSOR_W + c];
      if (validTemp(v)) { sum += v; n++; }
    }
  }
  if (n) s.centerT = sum / n;
  return s;
}

// Average of each pixel's 3x3 neighborhood. The markers go on the hottest and
// coldest of these, which sits in the middle of a warm or cold object instead
// of hopping between its pixels with noise.
static void boxAverage(const float *t, float *out) {
  for (int r = 0; r < SENSOR_H; r++) {
    for (int c = 0; c < SENSOR_W; c++) {
      float sum = 0;
      int n = 0;
      for (int rr = max(r - 1, 0); rr <= min(r + 1, SENSOR_H - 1); rr++) {
        for (int cc = max(c - 1, 0); cc <= min(c + 1, SENSOR_W - 1); cc++) {
          const float v = t[rr * SENSOR_W + cc];
          if (validTemp(v)) { sum += v; n++; }
        }
      }
      out[r * SENSOR_W + c] = n ? sum / n : NAN;
    }
  }
}

static void ease(float &value, float target, float rate) {
  value = isnan(value) ? target : value + (target - value) * rate;
}

// Smooths a new frame into smoothT, then updates the markers, readouts and
// color scale. Each eases more gently, so the screen settles instead of
// jumping with sensor noise.
static void processFrame(float *t) {
  repairPixels(t);
  const float follow = 1.0f - SMOOTHING;
  if (!haveFrame) memcpy(smoothT, t, sizeof(smoothT));
  for (int i = 0; i < SENSOR_PIXELS; i++) {
    if (!validTemp(t[i])) continue;
    if (validTemp(smoothT[i])) {
      smoothT[i] += (t[i] - smoothT[i]) * follow;
    } else {
      smoothT[i] = t[i];
    }
  }
  const FrameStats s = computeStats(smoothT);
  if (!isfinite(s.minT)) return;  // nothing valid yet
  haveFrame = true;

  // Markers only move to a spot that is clearly hotter / colder.
  static float box[SENSOR_PIXELS];
  boxAverage(smoothT, box);
  const FrameStats b = computeStats(box);
  if (shown.maxIdx < 0 || !validTemp(box[shown.maxIdx]) || box[b.maxIdx] > box[shown.maxIdx] + MARKER_HYSTERESIS) {
    shown.maxIdx = b.maxIdx;
  }
  if (shown.minIdx < 0 || !validTemp(box[shown.minIdx]) || box[b.minIdx] < box[shown.minIdx] - MARKER_HYSTERESIS) {
    shown.minIdx = b.minIdx;
  }
  markersVisible = s.maxT - s.minT >= 1.0f;

  ease(shown.centerT, s.centerT, follow * 0.6f);
  ease(shown.maxT, smoothT[shown.maxIdx], follow * 0.6f);
  ease(shown.minT, smoothT[shown.minIdx], follow * 0.6f);

  // The color scale follows the scene's min/max slowly.
  float lo = s.minT, hi = s.maxT;
  if (hi - lo < MIN_SPAN_C) {
    const float mid = (lo + hi) * 0.5f;
    lo = mid - MIN_SPAN_C * 0.5f;
    hi = mid + MIN_SPAN_C * 0.5f;
  }
  ease(rangeLo, lo, follow * 0.25f);
  ease(rangeHi, hi, follow * 0.25f);
}

// Upscale the 32x24 frame with bilinear interpolation and write it straight
// into the canvas frame buffer.
static void drawThermalImage(const float *temps, float lo, float hi) {
  static int16_t level[SENSOR_PIXELS];  // palette index * 16, 0..4080
  const float k = 4080.0f / (hi - lo);
  for (int i = 0; i < SENSOR_PIXELS; i++) {
    const float v = (temps[i] - lo) * k;
    level[i] = (v > 0) ? (v < 4080 ? (int16_t)v : 4080) : 0;  // NaN -> 0
  }

  const uint16_t *lut = paletteLut[paletteIdx];
  uint16_t *fb = gfx->getFramebuffer();
  int32_t rowStart = fbBase + imgX * fbDx + imgY * fbDy;
  int32_t rowLevels[SENSOR_W];
  for (int y = 0; y < imgH; y++, rowStart += fbDy) {
    const int16_t *r0 = &level[ySrc0[y] * SENSOR_W];
    const int16_t *r1 = &level[ySrc1[y] * SENSOR_W];
    const int32_t wy = yWeight[y];
    for (int c = 0; c < SENSOR_W; c++) rowLevels[c] = (r0[c] * (256 - wy) + r1[c] * wy) >> 8;

    uint16_t *p = fb + rowStart;
    for (int x = 0; x < imgW; x++, p += fbDx) {
      const int32_t wx = xWeight[x];
      const int32_t v = (rowLevels[xSrc0[x]] * (256 - wx) + rowLevels[xSrc1[x]] * wx) >> 8;
      *p = lut[v >> 4];
    }
  }
}

// ============================================================================
//  Wireless screen link
// ============================================================================

// Runs on the Wi-Fi task: keep it short.
static void onLinkReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  if (len < (int)sizeof(PktHeader) || data[0] != LINK_MAGIC || data[1] != LINK_VERSION) return;
  const uint8_t type = data[2];
  if (isCamera) {
    if (type == PKT_HELLO) lastHelloMs = millis();
    return;
  }
  if (type != PKT_META && type != PKT_PIXELS) return;
  // Stay with one camera; switch only after it has been quiet for 2 s.
  const uint32_t now = millis();
  if (linkCameraHeardMs && now - linkCameraHeardMs < 2000 && memcmp(info->src_addr, linkCameraMac, 6) != 0) return;
  memcpy(linkCameraMac, info->src_addr, 6);
  linkCameraHeardMs = now;

  portENTER_CRITICAL(&linkLock);
  if (type == PKT_META && len == (int)sizeof(PktMeta)) {
    memcpy(&linkMeta, data, sizeof(PktMeta));
  } else if (type == PKT_PIXELS && len >= (int)offsetof(PktPixels, centi)) {
    const PktPixels *p = (const PktPixels *)data;
    const int start = p->chunk * LINK_CHUNK_PIXELS;
    if (p->chunk < LINK_CHUNKS && p->count <= LINK_CHUNK_PIXELS && start + p->count <= SENSOR_PIXELS &&
        len == (int)offsetof(PktPixels, centi) + p->count * 2) {
      memcpy(&linkPixels[start], p->centi, p->count * 2);
      if (p->chunk == LINK_CHUNKS - 1) linkFrameReady = true;
    }
  }
  portEXIT_CRITICAL(&linkLock);
}

static void initLink() {
  if (!WIRELESS_SCREEN) return;
  WiFi.mode(WIFI_STA);
  esp_wifi_set_channel(WIRELESS_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW failed to start, wireless screen off");
    return;
  }
  esp_now_register_recv_cb(onLinkReceive);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = WIRELESS_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  linkReady = esp_now_add_peer(&peer) == ESP_OK;
}

static void linkSend(const void *data, size_t len) {
  for (int tries = 0; tries < 3; tries++) {
    if (esp_now_send(BROADCAST_MAC, (const uint8_t *)data, len) != ESP_ERR_ESPNOW_NO_MEM) return;
    delay(2);  // send queue full: give the radio a moment
  }
}

// Camera: a screen said hello in the last 3 s.
static bool screenListening() { return linkReady && isCamera && lastHelloMs && millis() - lastHelloMs < 3000; }

// Camera: send what is on screen. The screen draws it with the same code.
static void sendFrame() {
  static uint16_t frameNo = 0;
  frameNo++;
  PktMeta m = {};
  m.h = {LINK_MAGIC, LINK_VERSION, PKT_META, frameNo};
  m.palette = paletteIdx;
  m.flags = (fahrenheit ? 1 : 0) | (markersVisible ? 2 : 0);
  m.hotIdx = shown.maxIdx;
  m.coldIdx = shown.minIdx;
  m.centerT = shown.centerT;
  m.minT = shown.minT;
  m.maxT = shown.maxT;
  m.rangeLo = rangeLo;
  m.rangeHi = rangeHi;
  linkSend(&m, sizeof(m));

  PktPixels p;
  p.h = {LINK_MAGIC, LINK_VERSION, PKT_PIXELS, frameNo};
  for (int chunk = 0; chunk < LINK_CHUNKS; chunk++) {
    const int start = chunk * LINK_CHUNK_PIXELS;
    const int count = min(LINK_CHUNK_PIXELS, SENSOR_PIXELS - start);
    p.chunk = chunk;
    p.count = count;
    for (int i = 0; i < count; i++) {
      const float v = smoothT[start + i];
      p.centi[i] = validTemp(v) ? (int16_t)lroundf(v * 100.0f) : INT16_MIN;
    }
    linkSend(&p, offsetof(PktPixels, centi) + count * 2);
  }
}

// Screen: tell cameras nearby that someone is watching.
static void sendHello() {
  const PktHeader h = {LINK_MAGIC, LINK_VERSION, PKT_HELLO, 0};
  linkSend(&h, sizeof(h));
}

// Screen: take the newest frame from the camera, if one has arrived.
static bool takeLinkFrame() {
  static int16_t px[SENSOR_PIXELS];
  PktMeta m;
  portENTER_CRITICAL(&linkLock);
  const bool ready = linkFrameReady;
  if (ready) {
    m = linkMeta;
    memcpy(px, linkPixels, sizeof(px));
    linkFrameReady = false;
  }
  portEXIT_CRITICAL(&linkLock);
  if (!ready || m.h.magic != LINK_MAGIC) return false;

  for (int i = 0; i < SENSOR_PIXELS; i++) smoothT[i] = px[i] == INT16_MIN ? NAN : px[i] / 100.0f;
  paletteIdx = m.palette % PALETTE_COUNT;
  fahrenheit = m.flags & 1;
  markersVisible = m.flags & 2;
  shown.maxIdx = (m.hotIdx >= 0 && m.hotIdx < SENSOR_PIXELS) ? m.hotIdx : -1;
  shown.minIdx = (m.coldIdx >= 0 && m.coldIdx < SENSOR_PIXELS) ? m.coldIdx : -1;
  shown.centerT = m.centerT;
  shown.minT = m.minT;
  shown.maxT = m.maxT;
  rangeLo = m.rangeLo;
  rangeHi = max(m.rangeHi, m.rangeLo + 0.1f);
  haveFrame = true;
  return true;
}

// ============================================================================
//  Overlays
// ============================================================================

// Draws e.g. "23.4°C" and returns its width in pixels. draw=false only measures.
static int16_t drawTemp(int16_t x, int16_t y, float celsius, uint8_t size, uint16_t color, bool draw = true) {
  char num[12];
  if (isnan(celsius)) {
    strcpy(num, "--.-");
  } else {
    snprintf(num, sizeof(num), "%.1f", fahrenheit ? celsius * 1.8f + 32.0f : celsius);
  }
  const int16_t numW = strlen(num) * 6 * size;
  if (draw) {
    gfx->setTextSize(size);
    gfx->setTextColor(color);
    gfx->setCursor(x, y);
    gfx->print(num);
    gfx->drawCircle(x + numW + size + 1, y + size + 1, size, color);  // degree sign
    gfx->setCursor(x + numW + 4 * size, y);
    gfx->print(fahrenheit ? 'F' : 'C');
  }
  return numW + 10 * size;
}

static void drawCrosshair(int16_t cx, int16_t cy) {
  const int16_t gap = 4, len = 12;
  // Dark outline so it stays visible on white-hot areas.
  gfx->fillRect(cx - gap - len - 1, cy - 1, len + 2, 3, COLOR_BLACK);
  gfx->fillRect(cx + gap, cy - 1, len + 2, 3, COLOR_BLACK);
  gfx->fillRect(cx - 1, cy - gap - len - 1, 3, len + 2, COLOR_BLACK);
  gfx->fillRect(cx - 1, cy + gap, 3, len + 2, COLOR_BLACK);
  gfx->drawFastHLine(cx - gap - len, cy, len, COLOR_WHITE);
  gfx->drawFastHLine(cx + gap + 1, cy, len, COLOR_WHITE);
  gfx->drawFastVLine(cx, cy - gap - len, len, COLOR_WHITE);
  gfx->drawFastVLine(cx, cy + gap + 1, len, COLOR_WHITE);
}

// A colored ring with dark edges around one sensor pixel.
static void drawSpot(int idx, uint16_t color) {
  if (idx < 0) return;
  int col = idx % SENSOR_W, row = idx / SENSOR_W;
  if (MIRROR_IMAGE) col = SENSOR_W - 1 - col;
  if (FLIP_IMAGE) row = SENSOR_H - 1 - row;
  const int16_t cx = imgX + col * imgScale + imgScale / 2;
  const int16_t cy = imgY + row * imgScale + imgScale / 2;
  gfx->drawCircle(cx, cy, 10, COLOR_BLACK);
  gfx->drawCircle(cx, cy, 9, color);
  gfx->drawCircle(cx, cy, 8, color);
  gfx->drawCircle(cx, cy, 7, COLOR_BLACK);
}

// White text on a black box, right-aligned at `right`.
static void drawLabelRight(int16_t right, int16_t y, const char *text) {
  const int16_t tw = strlen(text) * 12;
  const int16_t tx = right - 6 - tw;
  gfx->fillRect(tx - 6, y, tw + 12, 28, COLOR_BLACK);
  gfx->setTextSize(2);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setCursor(tx, y + 6);
  gfx->print(text);
}

// Right edge for a top-right box at row y: inside the picture and the corner.
static int16_t topRightEdge(int16_t y) {
  return min<int16_t>(imgX + imgW - 6, scrW - cornerInset(y) - EDGE_PAD);
}

// Battery level with a bolt while charging, or "USB" when there is no battery.
// Right-aligned at `right`; returns its left edge.
static int16_t drawBattery(int16_t right, int16_t y) {
  if (!battKnown) return right;
  const bool present = battPresent;
  const int pct = battPercent;
  const bool bolt = battCharging || !present;
  char txt[6];
  if (!present) {
    strcpy(txt, "USB");
  } else if (pct < 0) {
    strcpy(txt, "--%");
  } else {
    snprintf(txt, sizeof(txt), "%d%%", pct);
  }
  const int16_t w = 6 + (bolt ? 14 : 0) + (present ? 33 : 0) + strlen(txt) * 12 + 6;
  int16_t x = right - w;
  gfx->fillRect(x, y, w, 28, COLOR_BLACK);
  x += 6;
  if (bolt) {
    gfx->fillTriangle(x + 7, y + 4, x, y + 15, x + 5, y + 15, COLOR_YELLOW);
    gfx->fillTriangle(x + 4, y + 13, x + 10, y + 13, x + 3, y + 24, COLOR_YELLOW);
    x += 14;
  }
  if (present) {
    gfx->drawRect(x, y + 7, 26, 14, COLOR_WHITE);
    gfx->fillRect(x + 26, y + 11, 3, 6, COLOR_WHITE);
    const int16_t fill = (22 * max(0, min(pct, 100)) + 50) / 100;
    if (fill > 0) gfx->fillRect(x + 2, y + 9, fill, 10, pct > 50 ? COLOR_GREEN : pct > 20 ? COLOR_YELLOW : COLOR_RED);
    x += 33;
  }
  gfx->setTextSize(2);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setCursor(x, y + 6);
  gfx->print(txt);
  return right - w;
}

static bool willPowerOff() { return pmuFound && battKnown && battPresent && !vbusPresent; }

// Centered over the picture, or with its top at `top` (below a text page).
static void drawIdleWarning(uint32_t msLeft, int16_t top = -1) {
  char line1[24];
  snprintf(line1, sizeof(line1), "%s in %u", willPowerOff() ? "Turning off" : "Screen off",
           (unsigned)((msLeft + 999) / 1000));
  static const char line2[] = "Tap screen to keep using";
  const int16_t w = max<int16_t>(strlen(line1) * 18, strlen(line2) * 12) + 32;
  const int16_t h = 14 + 24 + 12 + 16 + 14;
  const int16_t x = imgX + (imgW - w) / 2, y = top >= 0 ? top : imgY + (imgH - h) / 2;
  gfx->fillRect(x, y, w, h, COLOR_BLACK);
  gfx->drawRect(x, y, w, h, COLOR_WHITE);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(x + (w - strlen(line1) * 18) / 2, y + 14);
  gfx->print(line1);
  gfx->setTextSize(2);
  gfx->setCursor(x + (w - strlen(line2) * 12) / 2, y + 14 + 24 + 12);
  gfx->print(line2);
}

// forCapture leaves out the battery, notices and countdown.
static void drawOverlays(bool forCapture) {
  if (markersVisible) {
    drawSpot(shown.minIdx, COLOR_COLD);
    drawSpot(shown.maxIdx, COLOR_HOT);
  }
  drawCrosshair(imgX + imgW / 2, imgY + imgH / 2);

  // Center reading, top-left of the picture, clear of the rounded corner.
  const int16_t boxY = imgY + 8;
  const int16_t boxX = max<int16_t>(imgX + 6, cornerInset(boxY) + EDGE_PAD);
  const int16_t w = drawTemp(0, 0, shown.centerT, 3, COLOR_WHITE, false);
  gfx->fillRect(boxX, boxY, w + 12, 34, COLOR_BLACK);
  drawTemp(boxX + 6, boxY + 6, shown.centerT, 3, COLOR_WHITE);

  if (!forCapture) {
    // Battery top-right, "LIVE" next to it while a wireless screen watches;
    // notices (palette, units, saved picture) under them.
    const int16_t battLeft = drawBattery(topRightEdge(boxY), boxY);
    if (screenListening()) drawLabelRight(battLeft - 4, boxY, "LIVE");
    if ((int32_t)(toastUntil - millis()) > 0) drawLabelRight(topRightEdge(boxY + 34), boxY + 34, toastText);
    if (idleWarnLeftMs) drawIdleWarning(idleWarnLeftMs);
  }

  // Scale bar: coldest (blue, like its marker) | palette | hottest (red).
  const int16_t y0 = scrH - BAR_H;
  const int16_t textY = y0 + (BAR_H - 16) / 2;
  const int16_t sideX = textMargin(textY, 16);
  gfx->fillRect(0, y0, scrW, BAR_H, COLOR_BLACK);
  drawTemp(sideX, textY, shown.minT, 2, COLOR_COLD);
  const int16_t maxW = drawTemp(0, 0, shown.maxT, 2, COLOR_HOT, false);
  drawTemp(scrW - sideX - maxW, textY, shown.maxT, 2, COLOR_HOT);

  // Fixed position, so it doesn't jump around. 80 px fits "-40.0°F" and "572.0°F".
  const int16_t gx0 = sideX + 80 + 10, gx1 = scrW - sideX - 80 - 10;
  const uint16_t *lut = paletteLut[paletteIdx];
  for (int16_t x = gx0; x < gx1; x++) {
    gfx->drawFastVLine(x, y0 + 10, BAR_H - 20, lut[(x - gx0) * 255 / (gx1 - gx0 - 1)]);
  }
}

// ============================================================================
//  Saving pictures to the microSD card
// ============================================================================

static bool mountSd() {
  if (sdMounted) return true;
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  if (!SD_MMC.begin("/sdcard", true)) return false;  // 1-bit bus
  if (SD_MMC.cardType() == CARD_NONE) {
    SD_MMC.end();
    return false;
  }
  SD_MMC.mkdir("/thermal");
  sdMounted = true;
  return true;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static uint32_t get32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

// The screen exactly as shown (picture, readings and scale), as a 24-bit BMP.
static bool writeBmp(File &f) {
  const uint32_t rowBytes = (scrW * 3 + 3) & ~3u;
  const uint32_t imageBytes = rowBytes * scrH;
  uint8_t hdr[54] = {'B', 'M'};
  put32(hdr + 2, sizeof(hdr) + imageBytes);
  put32(hdr + 10, sizeof(hdr));
  put32(hdr + 14, 40);  // BITMAPINFOHEADER
  put32(hdr + 18, scrW);
  put32(hdr + 22, scrH);
  put16(hdr + 26, 1);
  put16(hdr + 28, 24);
  put32(hdr + 34, imageBytes);
  put32(hdr + 38, 2835);  // 72 dpi
  put32(hdr + 42, 2835);
  if (f.write(hdr, sizeof(hdr)) != sizeof(hdr)) return false;

  const int rowsPerWrite = 16;
  uint8_t *buf = (uint8_t *)malloc(rowBytes * rowsPerWrite);
  if (!buf) return false;
  const uint16_t *fb = gfx->getFramebuffer();
  bool ok = true;
  int rows = 0;
  for (int y = scrH - 1; y >= 0 && ok; y--) {  // BMP rows go bottom-up
    uint8_t *p = buf + rows * rowBytes;
    int32_t idx = fbBase + y * fbDy;
    for (int x = 0; x < scrW; x++, idx += fbDx) {
      const uint16_t c = fb[idx];
      const uint8_t r = c >> 11, g = (c >> 5) & 0x3F, b = c & 0x1F;
      *p++ = (b << 3) | (b >> 2);
      *p++ = (g << 2) | (g >> 4);
      *p++ = (r << 3) | (r >> 2);
    }
    memset(p, 0, rowBytes - scrW * 3);
    if (++rows == rowsPerWrite || y == 0) {
      ok = f.write(buf, rows * rowBytes) == rows * rowBytes;
      rows = 0;
    }
  }
  free(buf);
  return ok;
}

// The 32x24 temperatures in degrees C, oriented like the picture.
static bool writeCsv(File &f, const float *t) {
  char line[SENSOR_W * 9 + 2];
  for (int r = 0; r < SENSOR_H; r++) {
    const int sr = FLIP_IMAGE ? SENSOR_H - 1 - r : r;
    int len = 0;
    for (int c = 0; c < SENSOR_W; c++) {
      const int sc = MIRROR_IMAGE ? SENSOR_W - 1 - c : c;
      len += snprintf(line + len, sizeof(line) - len, c ? ",%.2f" : "%.2f", t[sr * SENSOR_W + sc]);
    }
    line[len++] = '\n';
    if (f.write((const uint8_t *)line, len) != (size_t)len) return false;
  }
  return true;
}

static void saveCapture(const float *temps) {
  if (!mountSd()) {
    showToast("No SD card");
    return;
  }
  uint16_t n = prefs.getUShort("shot", 1);
  char bmpPath[32], csvPath[32];
  for (;; n++) {
    snprintf(bmpPath, sizeof(bmpPath), "/thermal/IMG_%04u.bmp", n);
    if (!SD_MMC.exists(bmpPath)) break;
  }
  snprintf(csvPath, sizeof(csvPath), "/thermal/IMG_%04u.csv", n);

  File bmp = SD_MMC.open(bmpPath, FILE_WRITE);
  bool ok = bmp && writeBmp(bmp);
  if (bmp) bmp.close();
  File csv = ok ? SD_MMC.open(csvPath, FILE_WRITE) : File();
  ok = ok && csv && writeCsv(csv, temps);
  if (csv) csv.close();

  if (!ok) {
    Serial.printf("Writing %s failed\n", bmpPath);
    SD_MMC.end();  // remount next time, in case the card was swapped
    sdMounted = false;
    showToast("SD write failed");
    return;
  }
  prefs.putUShort("shot", n + 1);
  Serial.printf("Saved %s and %s\n", bmpPath, csvPath);
  char msg[24];
  snprintf(msg, sizeof(msg), "Saved IMG_%04u", n);
  showToast(msg);
}

// ============================================================================
//  Picture viewer
// ============================================================================

static void listPictures() {
  pictures.clear();
  if (!mountSd()) return;
  File dir = SD_MMC.open("/thermal");
  if (!dir || !dir.isDirectory()) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    unsigned n;
    char ext[5];
    if (sscanf(f.name(), "IMG_%u.%4s", &n, ext) == 2 && strcasecmp(ext, "bmp") == 0 && n <= 0xFFFF) {
      pictures.push_back(n);
    }
    f.close();
  }
  dir.close();
  std::sort(pictures.begin(), pictures.end());
}

// Decodes a 24-bit BMP into the frame buffer, centered.
static bool loadBmp(const char *path) {
  File f = SD_MMC.open(path, FILE_READ);
  if (!f) return false;
  uint8_t hdr[54];
  bool ok = f.read(hdr, sizeof(hdr)) == sizeof(hdr) && hdr[0] == 'B' && hdr[1] == 'M';
  const int32_t w = (int32_t)get32(hdr + 18), h = (int32_t)get32(hdr + 22);
  const int32_t rows = abs(h);
  ok = ok && (hdr[28] | (hdr[29] << 8)) == 24 && get32(hdr + 30) == 0 && w > 0 && w <= 4096 && rows > 0 &&
       rows <= 4096 && f.seek(get32(hdr + 10));
  const uint32_t rowBytes = (w * 3 + 3) & ~3u;
  uint8_t *row = ok ? (uint8_t *)malloc(rowBytes) : nullptr;
  if (!row) {
    f.close();
    return false;
  }
  gfx->fillScreen(COLOR_BLACK);
  uint16_t *fb = gfx->getFramebuffer();
  const int32_t ox = (scrW - w) / 2, oy = (scrH - rows) / 2;
  for (int32_t r = 0; r < rows && ok; r++) {
    ok = f.read(row, rowBytes) == rowBytes;
    const int32_t y = (h > 0 ? rows - 1 - r : r) + oy;  // BMP rows normally go bottom-up
    if (!ok || y < 0 || y >= scrH) continue;
    for (int32_t x = 0; x < w; x++) {
      const int32_t sx = x + ox;
      if (sx < 0 || sx >= scrW) continue;
      const uint8_t *p = row + x * 3;
      fb[fbBase + sx * fbDx + y * fbDy] = rgb565(p[2], p[1], p[0]);
    }
  }
  free(row);
  f.close();
  return ok;
}

static void showPicture() {
  viewerHasImage = false;
  viewerDirty = true;
  if (!sdMounted) {
    viewerNote = "No SD card.";
    return;
  }
  if (pictures.empty()) {
    viewerNote = "No pictures yet. Press PWR\nin the camera to save one.";
    return;
  }
  if (!viewerImage) viewerImage = (uint16_t *)ps_malloc(LCD_WIDTH * LCD_HEIGHT * 2);
  char path[32];
  snprintf(path, sizeof(path), "/thermal/IMG_%04u.bmp", pictures[viewIdx]);
  if (!viewerImage || !loadBmp(path)) {
    viewerNote = "Can't open this picture.";
    return;
  }
  memcpy(viewerImage, gfx->getFramebuffer(), LCD_WIDTH * LCD_HEIGHT * 2);
  viewerHasImage = true;
}

static void enterViewer() {
  mode = MODE_VIEWER;
  listPictures();
  viewIdx = pictures.empty() ? 0 : pictures.size() - 1;  // newest first
  showPicture();
}

static void exitViewer() {
  mode = MODE_CAMERA;
  screenDirty = true;
  waitingShown = false;
}

static void stepPicture(int delta) {
  if (pictures.empty()) return;
  const int n = pictures.size();
  viewIdx = (viewIdx + delta + n) % n;
  showPicture();
}

// Redraws the viewer when something changed, and a few times a second for
// the auto-off countdown.
static void drawViewer() {
  static uint32_t drawnAt = 0;
  if (!viewerDirty && millis() - drawnAt < 250) return;
  viewerDirty = false;
  drawnAt = millis();

  if (viewerHasImage) {
    memcpy(gfx->getFramebuffer(), viewerImage, LCD_WIDTH * LCD_HEIGHT * 2);
    char label[40];
    snprintf(label, sizeof(label), "IMG_%04u  %d/%d", pictures[viewIdx], viewIdx + 1, (int)pictures.size());
    drawLabelRight(topRightEdge(imgY + 8), imgY + 8, label);  // saved pictures leave this spot empty
  } else {
    drawMessage("Pictures", String(viewerNote) + "\n\nSwipe right to left to go\nback to the camera.");
  }
  if (idleWarnLeftMs) drawIdleWarning(idleWarnLeftMs, viewerHasImage ? -1 : WARNING_BELOW_TEXT);
  gfx->flush();
}

// ============================================================================
//  Auto-off
// ============================================================================

static void sleepScreen() {
  screenAsleep = true;
  idleWarnLeftMs = 0;
  panel->setBrightness(0);
  panel->displayOff();
}

static void wakeScreen() {
  screenAsleep = false;
  panel->displayOn();
  panel->setBrightness(SCREEN_BRIGHTNESS);
  screenDirty = true;
  viewerDirty = true;
  waitingShown = false;
}

static void noteActivity() {
  lastActivityMs = millis();
  if (screenAsleep) wakeScreen();
}

// On battery the board powers off (PWR turns it back on). On USB power only
// the screen goes off: with USB power present the power chip may switch
// straight back on.
static void turnOff() {
  if (willPowerOff()) {
    showMessage("Turning off", "Press PWR to turn on.");
    delay(1000);
    panel->setBrightness(0);
    if (boardTaskHandle) {
      powerOffRequested = true;  // the board task owns the I2C bus
    } else {
      powerOffNow();
    }
    delay(3000);
    powerOffRequested = false;  // still running: USB power must have appeared
  }
  sleepScreen();
}

// A wireless screen with no picture to show turns off sooner. Browsing saved
// pictures on it keeps the normal limit.
static uint32_t idleLimitMs() {
  const bool noSignal = !isCamera && mode == MODE_CAMERA && (!lastLinkFrameMs || millis() - lastLinkFrameMs > 1500);
  return (noSignal ? SCREEN_LINK_TIMEOUT_SECONDS : IDLE_OFF_SECONDS) * 1000UL;
}

static void manageIdle() {
  const bool wasWarning = idleWarnLeftMs > 0;
  idleWarnLeftMs = 0;
  const uint32_t limit = idleLimitMs();
  if (!screenAsleep && limit > 0) {
    const uint32_t idle = millis() - lastActivityMs;
    if (idle >= limit) {
      turnOff();
    } else if (idle + IDLE_WARNING_SECONDS * 1000UL >= limit) {
      idleWarnLeftMs = limit - idle;
    }
  }
  if (wasWarning != (idleWarnLeftMs > 0)) viewerDirty = true;
}

// ============================================================================
//  Buttons and touch
// ============================================================================

static void handleGesture(uint8_t g, int16_t x) {
  switch (g) {
    case GESTURE_SWIPE_RIGHT:
      if (mode == MODE_CAMERA) enterViewer();
      break;
    case GESTURE_SWIPE_LEFT:
      if (mode == MODE_VIEWER) exitViewer();
      break;
    case GESTURE_TAP:
      if (mode == MODE_VIEWER) stepPicture(x < scrW / 2 ? -1 : 1);
      break;
  }
}

static void pollButton() {
  static bool wasDown = false, holdHandled = false, wakePress = false;
  static uint32_t downSince = 0;
  const bool down = digitalRead(BOOT_BUTTON) == LOW;
  const uint32_t now = millis();

  if (down && !wasDown) {
    downSince = now;
    holdHandled = false;
    wakePress = screenAsleep;  // a press that wakes the screen does nothing else
    noteActivity();
  }
  if (wakePress) {
    wasDown = down;
    return;
  }
  if (!isCamera && mode == MODE_CAMERA) {  // palette and units follow the camera
    if (down && !wasDown) showToast("Set on the camera");
    wasDown = down;
    return;
  }
  if (down && !holdHandled && now - downSince >= 700) {
    holdHandled = true;
    if (mode == MODE_CAMERA) {
      fahrenheit = !fahrenheit;
      prefs.putBool("fahrenheit", fahrenheit);
      showToast(fahrenheit ? "Fahrenheit" : "Celsius");
    }
  }
  if (!down && wasDown && !holdHandled && now - downSince >= 30) {
    if (mode == MODE_VIEWER) {
      stepPicture(-1);
    } else {
      paletteIdx = (paletteIdx + 1) % PALETTE_COUNT;
      prefs.putUChar("palette", paletteIdx);
      showToast(PALETTES[paletteIdx].name);
    }
  }
  wasDown = down;
}

// ============================================================================

void setup() {
  Serial.begin(115200);
  pinMode(BOOT_BUTTON, INPUT_PULLUP);

  if (!initDisplay()) {
    for (;;) delay(1000);
  }
  setupLayout();
  for (uint8_t i = 0; i < PALETTE_COUNT; i++) buildPaletteLut(PALETTES[i], paletteLut[i]);

  prefs.begin("thermalcam", false);
  paletteIdx = prefs.getUChar("palette", 0) % PALETTE_COUNT;
  fahrenheit = prefs.getBool("fahrenheit", START_IN_FAHRENHEIT);

  initPower();
  pollBattery();
  initTouch();
  if (!mountSd()) Serial.println("No SD card");
  isCamera = startSensor();
  initLink();
  xTaskCreatePinnedToCore(boardTask, "board", 16384, nullptr, 2, &boardTaskHandle, 0);
  lastActivityMs = millis();  // count idle time from when the camera is running
}

void loop() {
  static float temps[SENSOR_PIXELS];
  static uint32_t lastFrameAt = millis();
  static bool stallShown = false;
  static uint32_t statsAt = millis(), statsFrames = 0;
  static bool captureRequested = false;
  static bool hadVbus = vbusPresent;
  static uint32_t wakeTouchAt = 0;

  // ---- Input ----
  pollButton();
  if (touchSeen) {
    touchSeen = false;
    if (screenAsleep || idleWarnLeftMs) wakeTouchAt = millis();  // this touch only wakes / keeps on
    noteActivity();
  }
  if (gesture != GESTURE_NONE) {
    const uint8_t g = gesture;
    gesture = GESTURE_NONE;
    // Ignore the touch that woke the screen or dismissed the countdown.
    if ((int32_t)(gestureStartMs - wakeTouchAt) > 150) handleGesture(g, gestureX);
  }
  if (powerKeyPressed) {
    powerKeyPressed = false;
    const bool woke = screenAsleep;
    noteActivity();
    if (!woke) {
      if (mode == MODE_VIEWER) {
        exitViewer();
      } else {
        captureRequested = true;
      }
    }
  }
  if (vbusPresent != hadVbus) {  // USB power plugged in or out
    hadVbus = vbusPresent;
    noteActivity();
  }
  if (screenListening()) lastActivityMs = millis();  // someone is watching remotely
  manageIdle();

  // ---- Frames keep flowing in every mode, so the smoothing stays current ----
  bool fresh;
  if (isCamera) {
    fresh = takeFrame(temps);
    if (fresh) {
      processFrame(temps);
      if (screenListening()) sendFrame();
    }
  } else {
    static uint32_t helloAt = 0;
    if (linkReady && millis() - helloAt >= 500) {
      helloAt = millis();
      sendHello();
    }
    fresh = takeLinkFrame();
    if (fresh) {
      noteActivity();  // a live picture keeps the screen on
      lastLinkFrameMs = millis();
      everConnected = true;
    }
  }
  if (fresh) {
    lastFrameAt = millis();
    stallShown = false;
    statsFrames++;
  }

  if (screenAsleep) {
    delay(2);
    return;
  }
  if (mode == MODE_VIEWER) {
    drawViewer();
    delay(2);
    return;
  }
  if (!isCamera && (!haveFrame || millis() - lastFrameAt > 1500)) {
    static uint32_t waitDrawnAt = 0;
    if (!waitingShown || millis() - waitDrawnAt >= 1000) {  // once a second for the countdown
      waitingShown = true;
      waitDrawnAt = millis();
      drawMessage("Wireless screen",
                  String(!linkReady       ? "Radio failed to start."
                         : everConnected ? "Lost the camera's signal.\nWaiting for it to come back."
                                         : "Waiting for the thermal camera.\nTurn it on nearby.") +
                      "\n\n" + sensorlessNote);
      if (idleWarnLeftMs) drawIdleWarning(idleWarnLeftMs, WARNING_BELOW_TEXT);
      gfx->flush();
      haveFrame = false;
    }
    delay(2);
    return;
  }
  if (!fresh || !haveFrame) {
    if (isCamera && !stallShown && millis() - lastFrameAt > 3000) {
      stallShown = true;
      char err[48] = "";
      if (sensorErrorCount) snprintf(err, sizeof(err), "\n(read error %d)", lastSensorError);
      showMessage("Sensor stopped", String("No data from the MLX90640.\nCheck the wires.") + err);
    }
    delay(2);
    return;
  }

  if (screenDirty) {
    gfx->fillScreen(COLOR_BLACK);
    screenDirty = false;
  }
  waitingShown = false;
  drawThermalImage(smoothT, rangeLo, rangeHi);
  drawOverlays(captureRequested);
  gfx->flush();
  if (captureRequested) {
    captureRequested = false;
    saveCapture(smoothT);
  }

  if (millis() - statsAt >= 5000) {
    Serial.printf("%.1f frames/s | center %.1f C | min %.1f | max %.1f | sensor errors %u\n",
                  statsFrames * 1000.0f / (millis() - statsAt), shown.centerT, shown.minT, shown.maxT,
                  (unsigned)sensorErrorCount);
    statsAt = millis();
    statsFrames = 0;
  }
}
