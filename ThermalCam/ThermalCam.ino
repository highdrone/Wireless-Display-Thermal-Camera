// Thermal camera: Waveshare MLX90640 thermal sensor (32x24 pixels) shown on a
// Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448 AMOLED). Works on both board
// revisions (SH8601 + FT3168, and V2 with CO5300 + CST820).
//
// BOOT button: short press = next color palette, hold = switch C / F.
// PWR button: short press = save a picture (BMP) and temperatures (CSV) to the
// microSD card.
// The screen flips to stay right side up, and the camera turns itself off
// after a minute without use (tap the screen to keep it on).
//
// Libraries (Arduino Library Manager):
//   "GFX Library for Arduino" by Moon On Our Nation
//   "Adafruit MLX90640" (installs "Adafruit BusIO" too)
// Board settings: see README.md (PSRAM must be set to "OPI PSRAM").

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <SD_MMC.h>
#include <Arduino_GFX_Library.h>
#include <Adafruit_MLX90640.h>

#include "config.h"
#include "palettes.h"

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

#define TCA9554_ADDR 0x20  // IO expander; EXIO0-2 are display/touch reset lines
#define FT3168_ADDR 0x38   // touch chip on the original board (SH8601 panel)
#define CST820_ADDR 0x15   // touch chip on the V2 board
#define QMI8658_ADDR_A 0x6B  // motion sensor (address depends on the board)
#define QMI8658_ADDR_B 0x6A
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
#define BAR_H 32                   // height of the scale bar under the picture
#define SCREEN_ROT (SCREEN_ROTATION & 3)
#define DEDICATED_BUS_HZ 800000    // ESP32-S3 I2C tops out around 800 kHz
#define EDGE_PAD 4                 // gap between text and the screen's curved edge

static const bool SENSOR_ON_BOARD_BUS = (THERMAL_SDA == BOARD_SDA && THERMAL_SCL == BOARD_SCL);

static const uint16_t COLOR_BLACK = 0x0000;
static const uint16_t COLOR_WHITE = 0xFFFF;
static const uint16_t COLOR_GREEN = 0x07E0;
static const uint16_t COLOR_YELLOW = 0xFFE0;
static const uint16_t COLOR_RED = 0xF800;

static Arduino_DataBus *bus;
static Arduino_OLED *panel;
static Arduino_Canvas *gfx;  // full-screen frame buffer in PSRAM, pushed with flush()
static Adafruit_MLX90640 mlx;
static Preferences prefs;

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
static float rangeLo = NAN, rangeHi = NAN;  // smoothed color scale, degrees C
static const char *boardName = "?";
static char toastText[24] = "";
static uint32_t toastUntil = 0;
static bool screenDirty = true;  // a message screen left text outside the picture
static bool boardIsV1 = false;
static bool pmuFound = false;
static bool sdMounted = false;
static uint8_t currentRot = SCREEN_ROT;  // screen rotation in use
static bool imgMirror = MIRROR_IMAGE, imgFlip = FLIP_IMAGE;  // picture orientation for currentRot
static uint8_t touchAddr = 0, imuAddr = 0;

// Written by the sensor task, which is the only user of the board's I2C bus
// once it runs, and by the touch interrupt.
static TaskHandle_t sensorTaskHandle = nullptr;
static volatile bool powerKeyPressed = false;
static volatile bool touchSeen = false;
static volatile uint8_t wantedRot = SCREEN_ROT;  // from the motion sensor
static volatile bool battKnown = false, battPresent = false, battCharging = false, vbusPresent = false;
static volatile int8_t battPercent = -1;
static volatile bool powerOffRequested = false;

// Auto-off.
static uint32_t lastActivityMs = 0;
static uint32_t idleWarnLeftMs = 0;  // > 0 while the countdown is showing
static bool screenAsleep = false;

struct FrameStats {
  float minT, maxT, centerT;
  int maxIdx;
};

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

// Runs on the sensor task: once it starts, it is the only user of the board's
// I2C bus, so the PMU reads can't interleave with sensor reads.
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

// Backs up the interrupt line: register 0x02 is the finger count on both chips.
static void pollTouch() {
  uint8_t n;
  if (touchAddr && i2cReadReg(touchAddr, 0x02, n) && (n & 0x0F) >= 1 && (n & 0x0F) <= 5) touchSeen = true;
}

static void initImu() {
  if (!AUTO_ROTATE) return;
  const uint8_t addrs[] = {QMI8658_ADDR_A, QMI8658_ADDR_B};
  for (uint8_t a : addrs) {
    uint8_t id;
    if (i2cReadReg(a, 0x00, id) && id == 0x05) {  // WHO_AM_I
      imuAddr = a;
      break;
    }
  }
  if (!imuAddr) {
    Serial.println("Motion sensor not found, auto-rotate off");
    return;
  }
  i2cWriteReg(imuAddr, 0x02, 0x40);  // CTRL1: auto-increment register address
  i2cWriteReg(imuAddr, 0x03, 0x17);  // CTRL2: accelerometer +-4 g, 62.5 Hz
  i2cWriteReg(imuAddr, 0x08, 0x01);  // CTRL7: accelerometer on, gyro off
}

// Picks the landscape rotation from gravity. The motion sensor's Y axis runs
// along the screen's short side (per Waveshare's tilt demo), so its sign says
// which way up the board is held. A change must hold for 3 readings in a row.
static void pollImu() {
  static uint8_t candidate = 0, streak = 0;
  uint8_t d[6];
  if (!imuAddr || !i2cReadRegs(imuAddr, 0x35, d, sizeof(d))) return;
  float gy = (int16_t)(d[2] | (d[3] << 8)) / 8192.0f;  // in g
  if (AUTO_ROTATE_INVERT) gy = -gy;
  uint8_t r;
  if (gy > 0.55f) {
    r = 1;
  } else if (gy < -0.55f) {
    r = 3;
  } else {
    streak = 0;  // lying flat or standing on end: keep the current way up
    return;
  }
  if (r != candidate) {
    candidate = r;
    streak = 0;
  }
  if (++streak >= 3) wantedRot = r;
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

  // The picture turns with the screen. A sensor fixed to the board already
  // turns with it, so then the picture is turned back to stay put.
  const bool turnedBack = SENSOR_FIXED_TO_BOARD && ((currentRot - SCREEN_ROT) & 3) == 2;
  imgMirror = MIRROR_IMAGE != turnedBack;
  imgFlip = FLIP_IMAGE != turnedBack;
  buildAxis(imgW, SENSOR_W, imgMirror, xSrc0, xSrc1, xWeight);
  buildAxis(imgH, SENSOR_H, imgFlip, ySrc0, ySrc1, yWeight);

  // Same mapping as Arduino_Canvas::writePixelPreclipped().
  switch (currentRot) {
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

// Full-screen text message, one line per '\n'.
static void showMessage(const char *title, const String &body) {
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
  gfx->flush();
  screenDirty = true;
}

static void showToast(const char *text) {
  strlcpy(toastText, text, sizeof(toastText));
  toastUntil = millis() + 1500;
}

// ============================================================================
//  Sensor
// ============================================================================

static void sensorTask(void *) {
  static float frame[SENSOR_PIXELS];
  uint32_t batteryPolledAt = 0;
  for (;;) {
    const int rc = mlx.getFrame(frame);  // reads both sub-pages
    if (rc == 0) {
      portENTER_CRITICAL(&frameLock);
      memcpy(sharedFrame, frame, sizeof(frame));
      sharedFrameNew = true;
      portEXIT_CRITICAL(&frameLock);
    } else {
      lastSensorError = rc;
      sensorErrorCount = sensorErrorCount + 1;
      vTaskDelay(pdMS_TO_TICKS(50));
    }
    pollPowerKey();
    pollTouch();
    pollImu();
    if (millis() - batteryPolledAt >= 2000) {
      batteryPolledAt = millis();
      pollBattery();
    }
    if (powerOffRequested) powerOffNow();
    vTaskDelay(1);
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

static void startSensor() {
  TwoWire &w = SENSOR_ON_BOARD_BUS ? Wire : Wire1;
  if (!SENSOR_ON_BOARD_BUS) w.begin(THERMAL_SDA, THERMAL_SCL, DEDICATED_BUS_HZ);

  showMessage("Thermal camera", "Starting sensor...");
  while (!mlx.begin(MLX90640_I2CADDR_DEFAULT, &w)) {
    const String seen = scanBus(w);
    Serial.printf("Display %s, PSRAM %u. MLX90640 not found on SDA=%d SCL=%d. I2C devices: %s\n",
                  boardName, (unsigned)ESP.getPsramSize(), THERMAL_SDA, THERMAL_SCL, seen.c_str());
    char where[40];
    snprintf(where, sizeof(where), "on SDA=GPIO%d, SCL=GPIO%d.", THERMAL_SDA, THERMAL_SCL);
    showMessage("Sensor not found",
                String("Looking for MLX90640 (0x33)\n") + where + "\nI2C devices seen:\n" + seen +
                    "\n\nCheck the 4 wires:\nVCC->3V3   GND->GND\nSDA->SDA   SCL->SCL\n\nRetrying...");
    delay(2000);
  }
  Serial.printf("MLX90640 found, serial %04X%04X%04X\n", mlx.serialNumber[0], mlx.serialNumber[1],
                mlx.serialNumber[2]);

  mlx.setMode(MLX90640_CHESS);
  mlx.setResolution(MLX90640_ADC_18BIT);
  mlx.setRefreshRate(SENSOR_REFRESH);

  xTaskCreatePinnedToCore(sensorTask, "thermal", 16384, nullptr, 2, &sensorTaskHandle, 0);
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
  FrameStats s = {INFINITY, -INFINITY, NAN, 0};
  for (int i = 0; i < SENSOR_PIXELS; i++) {
    const float v = t[i];
    if (!validTemp(v)) continue;
    if (v < s.minT) s.minT = v;
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

// Auto range: follow the scene's min/max, smoothed so the colors don't flicker.
static void updateRange(const FrameStats &s) {
  float lo = s.minT, hi = s.maxT;
  if (hi - lo < MIN_SPAN_C) {
    const float mid = (lo + hi) * 0.5f;
    lo = mid - MIN_SPAN_C * 0.5f;
    hi = mid + MIN_SPAN_C * 0.5f;
  }
  if (isnan(rangeLo)) {
    rangeLo = lo;
    rangeHi = hi;
  } else {
    rangeLo += (lo - rangeLo) * 0.3f;
    rangeHi += (hi - rangeHi) * 0.3f;
  }
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

static void drawHotspot(int idx) {
  int col = idx % SENSOR_W, row = idx / SENSOR_W;
  if (imgMirror) col = SENSOR_W - 1 - col;
  if (imgFlip) row = SENSOR_H - 1 - row;
  const int16_t cx = imgX + col * imgScale + imgScale / 2;
  const int16_t cy = imgY + row * imgScale + imgScale / 2;
  gfx->drawCircle(cx, cy, 9, COLOR_BLACK);
  gfx->drawCircle(cx, cy, 8, COLOR_WHITE);
  gfx->drawCircle(cx, cy, 7, COLOR_BLACK);
}

// Battery level with a bolt while charging, or "USB" when there is no battery.
// Right-aligned at `right`.
static void drawBattery(int16_t right, int16_t y) {
  if (!battKnown) return;
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
}

static bool willPowerOff() { return pmuFound && battKnown && battPresent && !vbusPresent; }

static void drawIdleWarning(uint32_t msLeft) {
  char line1[24];
  snprintf(line1, sizeof(line1), "%s in %u", willPowerOff() ? "Turning off" : "Screen off",
           (unsigned)((msLeft + 999) / 1000));
  static const char line2[] = "Tap screen to keep using";
  const int16_t w = max<int16_t>(strlen(line1) * 18, strlen(line2) * 12) + 32;
  const int16_t h = 14 + 24 + 12 + 16 + 14;
  const int16_t x = imgX + (imgW - w) / 2, y = imgY + (imgH - h) / 2;
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
static void drawOverlays(const FrameStats &s, bool forCapture) {
  if (s.maxT - s.minT >= 1.0f) drawHotspot(s.maxIdx);
  drawCrosshair(imgX + imgW / 2, imgY + imgH / 2);

  // Center reading, top-left of the picture, clear of the rounded corner.
  const int16_t boxY = imgY + 8;
  const int16_t boxX = max<int16_t>(imgX + 6, cornerInset(boxY) + EDGE_PAD);
  const int16_t w = drawTemp(0, 0, s.centerT, 3, COLOR_WHITE, false);
  gfx->fillRect(boxX, boxY, w + 12, 34, COLOR_BLACK);
  drawTemp(boxX + 6, boxY + 6, s.centerT, 3, COLOR_WHITE);

  if (!forCapture) {
    // Battery top-right; notices (palette, units, saved picture) under it.
    drawBattery(min<int16_t>(imgX + imgW - 6, scrW - cornerInset(boxY) - EDGE_PAD), boxY);
    if ((int32_t)(toastUntil - millis()) > 0) {
      const int16_t toastY = boxY + 34;
      const int16_t tw = strlen(toastText) * 12;
      const int16_t tx = min<int16_t>(imgX + imgW - 6, scrW - cornerInset(toastY) - EDGE_PAD) - 6 - tw;
      gfx->fillRect(tx - 6, toastY, tw + 12, 28, COLOR_BLACK);
      gfx->setTextSize(2);
      gfx->setTextColor(COLOR_WHITE);
      gfx->setCursor(tx, toastY + 6);
      gfx->print(toastText);
    }
    if (idleWarnLeftMs) drawIdleWarning(idleWarnLeftMs);
  }

  // Scale bar: min | palette | max.
  const int16_t y0 = scrH - BAR_H;
  const int16_t textY = y0 + (BAR_H - 16) / 2;
  const int16_t sideX = textMargin(textY, 16);
  gfx->fillRect(0, y0, scrW, BAR_H, COLOR_BLACK);
  drawTemp(sideX, textY, s.minT, 2, COLOR_WHITE);
  const int16_t maxW = drawTemp(0, 0, s.maxT, 2, COLOR_WHITE, false);
  drawTemp(scrW - sideX - maxW, textY, s.maxT, 2, COLOR_WHITE);

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
    const int sr = imgFlip ? SENSOR_H - 1 - r : r;
    int len = 0;
    for (int c = 0; c < SENSOR_W; c++) {
      const int sc = imgMirror ? SENSOR_W - 1 - c : c;
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
//  Rotation and auto-off
// ============================================================================

static void applyRotation(uint8_t r) {
  currentRot = r;
  gfx->setRotation(r);
  setupLayout();
  screenDirty = true;
}

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
    if (sensorTaskHandle) {
      powerOffRequested = true;  // the sensor task owns the I2C bus
    } else {
      powerOffNow();
    }
    delay(3000);
    powerOffRequested = false;  // still running: USB power must have appeared
  }
  sleepScreen();
}

static void manageIdle() {
  idleWarnLeftMs = 0;
  if (screenAsleep || IDLE_OFF_SECONDS <= 0) return;
  const uint32_t idle = millis() - lastActivityMs;
  const uint32_t limit = IDLE_OFF_SECONDS * 1000UL;
  if (idle >= limit) {
    turnOff();
  } else if (idle + IDLE_WARNING_SECONDS * 1000UL >= limit) {
    idleWarnLeftMs = limit - idle;
  }
}

// ============================================================================
//  Button
// ============================================================================

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
  if (down && !holdHandled && now - downSince >= 700) {
    holdHandled = true;
    fahrenheit = !fahrenheit;
    prefs.putBool("fahrenheit", fahrenheit);
    showToast(fahrenheit ? "Fahrenheit" : "Celsius");
  }
  if (!down && wasDown && !holdHandled && now - downSince >= 30) {
    paletteIdx = (paletteIdx + 1) % PALETTE_COUNT;
    prefs.putUChar("palette", paletteIdx);
    showToast(PALETTES[paletteIdx].name);
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
  initImu();
  for (int i = 0; i < 3; i++) pollImu();  // start the right way up
  if (wantedRot != currentRot) applyRotation(wantedRot);
  if (!mountSd()) Serial.println("No SD card");
  lastActivityMs = millis();
  startSensor();
}

void loop() {
  static float temps[SENSOR_PIXELS];
  static uint32_t lastFrameAt = millis();
  static bool stallShown = false;
  static uint32_t statsAt = millis(), statsFrames = 0;
  static bool captureRequested = false;
  static bool hadVbus = vbusPresent;

  pollButton();
  if (touchSeen) {
    touchSeen = false;
    noteActivity();
  }
  if (powerKeyPressed) {
    powerKeyPressed = false;
    if (!screenAsleep) captureRequested = true;  // a press that wakes the screen doesn't capture
    noteActivity();
  }
  if (vbusPresent != hadVbus) {  // USB power plugged in or out
    hadVbus = vbusPresent;
    noteActivity();
  }
  if (wantedRot != currentRot) {
    applyRotation(wantedRot);
    noteActivity();
  }
  manageIdle();

  if (!takeFrame(temps)) {
    if (!stallShown && !screenAsleep && millis() - lastFrameAt > 3000) {
      stallShown = true;
      char err[48] = "";
      if (sensorErrorCount) snprintf(err, sizeof(err), "\n(read error %d)", lastSensorError);
      showMessage("Sensor stopped", String("No data from the MLX90640.\nCheck the wires.") + err);
    }
    delay(2);
    return;
  }
  lastFrameAt = millis();
  stallShown = false;

  repairPixels(temps);
  const FrameStats s = computeStats(temps);
  if (!isfinite(s.minT)) return;  // nothing valid in this frame
  updateRange(s);

  if (screenAsleep) return;

  if (screenDirty) {
    gfx->fillScreen(COLOR_BLACK);
    screenDirty = false;
  }
  drawThermalImage(temps, rangeLo, rangeHi);
  drawOverlays(s, captureRequested);
  gfx->flush();
  if (captureRequested) {
    captureRequested = false;
    saveCapture(temps);
  }

  statsFrames++;
  if (millis() - statsAt >= 5000) {
    Serial.printf("%.1f fps | center %.1f C | min %.1f | max %.1f | sensor errors %u\n",
                  statsFrames * 1000.0f / (millis() - statsAt), s.centerT, s.minT, s.maxT,
                  (unsigned)sensorErrorCount);
    statsAt = millis();
    statsFrames = 0;
  }
}
