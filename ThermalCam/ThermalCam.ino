// Thermal camera: Waveshare MLX90640 thermal sensor (32x24 pixels) shown on a
// Waveshare ESP32-S3-Touch-AMOLED-1.8 (368x448 AMOLED). Works on both board
// revisions (SH8601 + FT3168, and V2 with CO5300 + CST820).
//
// BOOT button: short press = next color palette, hold = switch C / F.
//
// Libraries (Arduino Library Manager):
//   "GFX Library for Arduino" by Moon On Our Nation
//   "Adafruit MLX90640" (installs "Adafruit BusIO" too)
// Board settings: see README.md (PSRAM must be set to "OPI PSRAM").

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
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

#define TCA9554_ADDR 0x20  // IO expander; EXIO0-2 are display/touch reset lines
#define FT3168_ADDR 0x38   // touch chip on the original board (SH8601 panel)

#define SENSOR_W 32
#define SENSOR_H 24
#define SENSOR_PIXELS (SENSOR_W * SENSOR_H)
#define BAR_H 32                   // height of the scale bar under the picture
#define SCREEN_ROT (SCREEN_ROTATION & 3)
#define DEDICATED_BUS_HZ 800000    // ESP32-S3 I2C tops out around 800 kHz

static const bool SENSOR_ON_BOARD_BUS = (THERMAL_SDA == BOARD_SDA && THERMAL_SCL == BOARD_SCL);

static const uint16_t COLOR_BLACK = 0x0000;
static const uint16_t COLOR_WHITE = 0xFFFF;

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

static bool i2cReadReg(uint8_t addr, uint8_t reg, uint8_t &val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return false;
  if (Wire.requestFrom(addr, (uint8_t)1) != 1) return false;
  val = Wire.read();
  return true;
}

static void i2cWriteReg(uint8_t addr, uint8_t reg, uint8_t val) {
  Wire.beginTransmission(addr);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission();
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

// Full-screen text message, one line per '\n'.
static void showMessage(const char *title, const String &body) {
  gfx->fillScreen(COLOR_BLACK);
  gfx->setTextColor(COLOR_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(16, 24);
  gfx->print(title);
  gfx->setTextSize(2);
  int16_t y = 76;
  int start = 0;
  while (start <= (int)body.length()) {
    int end = body.indexOf('\n', start);
    if (end < 0) end = body.length();
    gfx->setCursor(16, y);
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

  xTaskCreatePinnedToCore(sensorTask, "thermal", 16384, nullptr, 2, nullptr, 0);
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
  if (MIRROR_IMAGE) col = SENSOR_W - 1 - col;
  if (FLIP_IMAGE) row = SENSOR_H - 1 - row;
  const int16_t cx = imgX + col * imgScale + imgScale / 2;
  const int16_t cy = imgY + row * imgScale + imgScale / 2;
  gfx->drawCircle(cx, cy, 9, COLOR_BLACK);
  gfx->drawCircle(cx, cy, 8, COLOR_WHITE);
  gfx->drawCircle(cx, cy, 7, COLOR_BLACK);
}

static void drawOverlays(const FrameStats &s) {
  if (s.maxT - s.minT >= 1.0f) drawHotspot(s.maxIdx);
  drawCrosshair(imgX + imgW / 2, imgY + imgH / 2);

  // Center reading, top-left of the picture.
  const int16_t w = drawTemp(0, 0, s.centerT, 3, COLOR_WHITE, false);
  gfx->fillRect(imgX + 6, imgY + 6, w + 12, 34, COLOR_BLACK);
  drawTemp(imgX + 12, imgY + 12, s.centerT, 3, COLOR_WHITE);

  // Palette / unit change notice, top-right.
  if ((int32_t)(toastUntil - millis()) > 0) {
    const int16_t tw = strlen(toastText) * 12;
    const int16_t tx = imgX + imgW - tw - 12;
    gfx->fillRect(tx - 6, imgY + 6, tw + 12, 28, COLOR_BLACK);
    gfx->setTextSize(2);
    gfx->setTextColor(COLOR_WHITE);
    gfx->setCursor(tx, imgY + 12);
    gfx->print(toastText);
  }

  // Scale bar: min | palette | max.
  const int16_t y0 = scrH - BAR_H;
  const int16_t textY = y0 + (BAR_H - 16) / 2;
  gfx->fillRect(0, y0, scrW, BAR_H, COLOR_BLACK);
  drawTemp(6, textY, s.minT, 2, COLOR_WHITE);
  const int16_t maxW = drawTemp(0, 0, s.maxT, 2, COLOR_WHITE, false);
  drawTemp(scrW - 6 - maxW, textY, s.maxT, 2, COLOR_WHITE);

  const int16_t gx0 = 6 + 96, gx1 = scrW - 6 - 96;  // fixed, so it doesn't jump around
  const uint16_t *lut = paletteLut[paletteIdx];
  for (int16_t x = gx0; x < gx1; x++) {
    gfx->drawFastVLine(x, y0 + 10, BAR_H - 20, lut[(x - gx0) * 255 / (gx1 - gx0 - 1)]);
  }
}

// ============================================================================
//  Button
// ============================================================================

static void pollButton() {
  static bool wasDown = false, holdHandled = false;
  static uint32_t downSince = 0;
  const bool down = digitalRead(BOOT_BUTTON) == LOW;
  const uint32_t now = millis();

  if (down && !wasDown) {
    downSince = now;
    holdHandled = false;
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

  startSensor();
}

void loop() {
  static float temps[SENSOR_PIXELS];
  static uint32_t lastFrameAt = millis();
  static bool stallShown = false;
  static uint32_t statsAt = millis(), statsFrames = 0;

  pollButton();

  if (!takeFrame(temps)) {
    if (!stallShown && millis() - lastFrameAt > 3000) {
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

  if (screenDirty) {
    gfx->fillScreen(COLOR_BLACK);
    screenDirty = false;
  }
  drawThermalImage(temps, rangeLo, rangeHi);
  drawOverlays(s);
  gfx->flush();

  statsFrames++;
  if (millis() - statsAt >= 5000) {
    Serial.printf("%.1f fps | center %.1f C | min %.1f | max %.1f | sensor errors %u\n",
                  statsFrames * 1000.0f / (millis() - statsAt), s.centerT, s.minT, s.maxT,
                  (unsigned)sensorErrorCount);
    statsAt = millis();
    statsFrames = 0;
  }
}
