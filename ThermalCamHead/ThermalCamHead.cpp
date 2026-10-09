// Thermal + color camera head for the Heltec HT-HC33 (ESP32-S3R8 with an
// OV3660 color camera), with a Waveshare MLX90640 thermal sensor soldered on.
// It has no display: it streams the thermal frames and small color JPEG
// pictures over ESP-NOW to a wireless screen (a Waveshare
// ESP32-S3-Touch-AMOLED-1.8 running ThermalCam), which lays the two on top
// of each other. The board's Wi-Fi HaLow module is not used and is held in
// reset.
//
// USER key: press to switch the head off, press again to turn it on.
// Wiring and settings: config.h and README.md ("Thermal + color camera head").
// Board settings (Arduino): ESP32S3 Dev Module, Flash Size 8MB, PSRAM "OPI
// PSRAM", Partition Scheme "8M with spiffs (3MB APP/1.5MB SPIFFS)".

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_camera.h>
#include <esp_sleep.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

#include "config.h"
#include "link_protocol.h"
#include "src/mlx90640/mlx90640.h"

// ---- HT-HC33 pins (Heltec's board definition) ------------------------------
#define USER_KEY 0
#define HALOW_RESET_N 8  // HT-HC01 Wi-Fi HaLow module: low = held in reset
#define CAM_PWDN 20
#define CAM_XCLK 47
#define CAM_SIOD 45
#define CAM_SIOC 42
#define CAM_D0 17
#define CAM_D1 13
#define CAM_D2 12
#define CAM_D3 14
#define CAM_D4 18
#define CAM_D5 46
#define CAM_D6 48
#define CAM_D7 38
#define CAM_VSYNC 40
#define CAM_HREF 39
#define CAM_PCLK 21

#define MLX_ADDR 0x33
#define SENSOR_W 32
#define SENSOR_H 24
#define SENSOR_PIXELS (SENSOR_W * SENSOR_H)
#define EMISSIVITY 0.95f
#define TA_SHIFT 8.0f            // reflected temperature = sensor temperature - 8 C (open air)
#define MARKER_HYSTERESIS 0.2f   // C a spot must beat the marked one by to take the marker
#define SENSOR_BUS_HZ 800000     // the sensor has this I2C bus to itself
#define LISTEN_MS 3000           // a screen counts as listening this long after its hello

static_assert(LINK_SENSOR_PIXELS == SENSOR_PIXELS, "link frame size");

// ---- State -----------------------------------------------------------------
static paramsMLX90640 mlxParams;
static bool sensorOk = false, cameraOk = false;
static int sensorSda = -1, sensorScl = -1;
static esp_err_t cameraError = ESP_OK;

static float sharedFrame[SENSOR_PIXELS];  // newest full frame from the sensor task
static bool sharedFrameNew = false;
static portMUX_TYPE frameLock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t sensorTaskHandle = nullptr;
static volatile bool sensorPause = false, sensorPaused = false;
static volatile uint32_t sensorErrors = 0;

struct FrameStats {
  float minT, maxT, centerT;
  int minIdx, maxIdx;
};
static float smoothT[SENSOR_PIXELS];
static bool haveFrame = false;
static FrameStats shown = {NAN, NAN, NAN, -1, -1};
static bool markersVisible = false;
static float rangeLo = NAN, rangeHi = NAN;

static const uint8_t BROADCAST_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
static bool linkReady = false;
static size_t jpegChunk = 220;                 // bytes of JPEG per packet (1400 with ESP-NOW v2)
static volatile uint32_t lastHelloMs = 0;      // when a switched-on screen last said hello
static volatile uint32_t colorWantedMs = 0;    // ... and last asked for color pictures
static volatile bool probeReplyDue = false;    // a screen in standby asked "are you on?"
static uint32_t screenSeenMs = 0;              // when a screen was last listening (or startup)

// Standby, kept in RTC memory through deep sleep.
RTC_DATA_ATTR static bool rtcStandby = false;
RTC_DATA_ATTR static uint32_t rtcStandbyChecks = 0;

// ============================================================================
//  Thermal sensor
// ============================================================================

static uint8_t refreshCode(int hz) {
  return hz >= 32 ? 0x06 : hz >= 16 ? 0x05 : hz >= 8 ? 0x04 : hz >= 4 ? 0x03 : 0x02;
}

static bool probeSensor(int sda, int scl) {
  Wire.begin(sda, scl, 100000);
  delay(5);
  bool found = false;
  for (int i = 0; i < 3 && !found; i++) {
    Wire.beginTransmission(MLX_ADDR);
    found = Wire.endTransmission() == 0;
    if (!found) delay(20);
  }
  if (!found) {
    Wire.end();
    pinMode(sda, INPUT);
    pinMode(scl, INPUT);
  }
  return found;
}

// Looks for the sensor on the configured pins, then with SDA/SCL swapped,
// then on the other SD-slot pair, and sets it up.
static bool startSensor() {
  const int pairs[4][2] = {{THERMAL_SDA, THERMAL_SCL},
                           {THERMAL_SCL, THERMAL_SDA},
                           {THERMAL_ALT_SDA, THERMAL_ALT_SCL},
                           {THERMAL_ALT_SCL, THERMAL_ALT_SDA}};
  for (auto &p : pairs) {
    if (!probeSensor(p[0], p[1])) continue;
    sensorSda = p[0];
    sensorScl = p[1];
    Wire.setClock(SENSOR_BUS_HZ);
    MLX90640_SetWire(&Wire);
    static uint16_t eeprom[MLX90640_EEPROM_DUMP_NUM];
    if (MLX90640_DumpEE(MLX_ADDR, eeprom) != 0) break;
    const int rc = MLX90640_ExtractParameters(eeprom, &mlxParams);
    if (rc == -MLX90640_EEPROM_DATA_ERROR) {
      Serial.println("MLX90640 calibration data invalid (is it an MLX90641?)");
      break;
    }
    MLX90640_SetChessMode(MLX_ADDR);
    MLX90640_SetResolution(MLX_ADDR, 0x02);  // 18-bit ADC
    MLX90640_SetRefreshRate(MLX_ADDR, refreshCode(SENSOR_REFRESH_HZ));
#if LOG_SENSOR_SERIAL
    uint16_t serial[3] = {0, 0, 0};
    MLX90640_I2CRead(MLX_ADDR, 0x2407, 3, serial);
    Serial.printf("MLX90640 found on SDA=GPIO%d SCL=GPIO%d, serial %04X%04X%04X\n", sensorSda, sensorScl, serial[0],
                  serial[1], serial[2]);
#else
    Serial.printf("MLX90640 found on SDA=GPIO%d SCL=GPIO%d\n", sensorSda, sensorScl);
#endif
    return true;
  }
  Serial.println("MLX90640 not found");
  return false;
}

// Core 0: reads each sub-page as soon as the sensor has it.
static void sensorTask(void *) {
  static uint16_t raw[834];
  static float frame[SENSOR_PIXELS];  // each sub-page refreshes half the pixels (chess pattern)
  uint8_t subpagesSeen = 0;
  for (;;) {
    if (sensorPause) {  // about to deep-sleep: leave the bus idle
      sensorPaused = true;
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    uint16_t status;
    if (MLX90640_I2CRead(MLX_ADDR, MLX90640_STATUS_REG, 1, &status) != 0) {
      sensorErrors = sensorErrors + 1;
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }
    if (MLX90640_GET_DATA_READY(status)) {
      const int subpage = MLX90640_GetFrameData(MLX_ADDR, raw);
      if (subpage < 0) {
        sensorErrors = sensorErrors + 1;
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
    vTaskDelay(pdMS_TO_TICKS(4));
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

// ============================================================================
//  Frame processing (the same as ThermalCam's, so the screen shows the same)
// ============================================================================

static inline bool validTemp(float v) { return v > -50.0f && v < 350.0f; }  // false for NaN

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
  if (!isfinite(s.minT)) return;
  haveFrame = true;

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

  float lo = s.minT, hi = s.maxT;
  if (hi - lo < MIN_SPAN_C) {
    const float mid = (lo + hi) * 0.5f;
    lo = mid - MIN_SPAN_C * 0.5f;
    hi = mid + MIN_SPAN_C * 0.5f;
  }
  ease(rangeLo, lo, follow * 0.25f);
  ease(rangeHi, hi, follow * 0.25f);
}

// ============================================================================
//  Color camera
// ============================================================================

static bool startCamera() {
  camera_config_t c = {};
  c.pin_pwdn = CAM_PWDN;
  c.pin_reset = -1;
  c.pin_xclk = CAM_XCLK;
  c.pin_sccb_sda = CAM_SIOD;
  c.pin_sccb_scl = CAM_SIOC;
  c.pin_d7 = CAM_D7;
  c.pin_d6 = CAM_D6;
  c.pin_d5 = CAM_D5;
  c.pin_d4 = CAM_D4;
  c.pin_d3 = CAM_D3;
  c.pin_d2 = CAM_D2;
  c.pin_d1 = CAM_D1;
  c.pin_d0 = CAM_D0;
  c.pin_vsync = CAM_VSYNC;
  c.pin_href = CAM_HREF;
  c.pin_pclk = CAM_PCLK;
  c.xclk_freq_hz = 20000000;
  c.ledc_timer = LEDC_TIMER_0;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = COLOR_FRAME_SIZE;
  c.jpeg_quality = COLOR_JPEG_QUALITY;
  c.fb_count = 2;
  c.fb_location = psramFound() ? CAMERA_FB_IN_PSRAM : CAMERA_FB_IN_DRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  c.sccb_i2c_port = 1;  // the thermal sensor uses I2C port 0 (Wire)
  cameraError = esp_camera_init(&c);
  if (cameraError != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", cameraError);
    return false;
  }
  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    s->set_vflip(s, COLOR_VFLIP ? 1 : 0);
    s->set_hmirror(s, COLOR_HMIRROR ? 1 : 0);
    Serial.printf("Camera sensor PID 0x%04x\n", s->id.PID);
  }
  return true;
}

// ============================================================================
//  Radio link
// ============================================================================

// Runs on the Wi-Fi task: keep it short. Only screens' hellos matter here.
static void onLinkReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
  (void)info;
  if (len < (int)sizeof(PktHeader) || data[0] != LINK_MAGIC || data[1] != LINK_VERSION || data[2] != PKT_HELLO) return;
  const uint16_t f = ((const PktHeader *)data)->frame;
  if (f == HELLO_PROBE) {
    probeReplyDue = true;  // one frame back, so a screen in standby wakes up
  } else {
    const uint32_t now = millis();
    lastHelloMs = now ? now : 1;
    if (f & HELLO_WANT_COLOR) colorWantedMs = lastHelloMs;
  }
}

static void initLink() {
  if (linkReady) return;
  WiFi.mode(WIFI_STA);
  esp_wifi_set_ps(WIFI_PS_NONE);  // hear every hello
  esp_wifi_set_channel(WIRELESS_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW failed to start");
    return;
  }
  esp_now_register_recv_cb(onLinkReceive);
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST_MAC, 6);
  peer.channel = WIRELESS_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  linkReady = esp_now_add_peer(&peer) == ESP_OK;
  esp_now_rate_config_t rate = {};
  rate.phymode = WIFI_PHY_MODE_11B;
  rate.rate = RADIO_RATE;
  if (RADIO_RATE == WIFI_PHY_RATE_24M || RADIO_RATE == WIFI_PHY_RATE_12M || RADIO_RATE == WIFI_PHY_RATE_6M) {
    rate.phymode = WIFI_PHY_MODE_11G;
  }
  if (esp_now_set_peer_rate_config(BROADCAST_MAC, &rate) != ESP_OK) Serial.println("Radio rate not set");
  uint32_t version = 1;
  esp_now_get_version(&version);
  jpegChunk = version >= 2 ? 1400 : 220;
  Serial.printf("ESP-NOW v%u, %u-byte picture packets\n", (unsigned)version, (unsigned)jpegChunk);
}

// Retries while the radio's send queue is full.
static bool linkSend(const void *data, size_t len) {
  for (int tries = 0; tries < 40; tries++) {
    const esp_err_t err = esp_now_send(BROADCAST_MAC, (const uint8_t *)data, len);
    if (err != ESP_ERR_ESPNOW_NO_MEM) return err == ESP_OK;
    delay(1);
  }
  return false;
}

static bool screenListening() { return linkReady && lastHelloMs && millis() - lastHelloMs < LISTEN_MS; }
static bool colorWanted() { return colorWantedMs && millis() - colorWantedMs < LISTEN_MS; }

static void sendFrame() {
  static uint16_t frameNo = 0;
  frameNo++;
  PktMeta m = {};
  m.h = {LINK_MAGIC, LINK_VERSION, PKT_META, frameNo};
  m.palette = 0;
  m.flags = META_HEADLESS | (markersVisible ? META_MARKERS : 0);
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

static void sendJpeg(const uint8_t *jpg, size_t len) {
  static uint16_t pictureNo = 0;
  static uint8_t pkt[sizeof(PktJpegHead) + 1400];
  if (len == 0 || len > LINK_JPEG_MAX) return;
  pictureNo++;
  PktJpegHead *h = (PktJpegHead *)pkt;
  h->h = {LINK_MAGIC, LINK_VERSION, PKT_JPEG, pictureNo};
  h->total = len;
  for (size_t off = 0; off < len; off += jpegChunk) {
    const size_t n = min(jpegChunk, len - off);
    h->offset = off;
    memcpy(pkt + sizeof(PktJpegHead), jpg + off, n);
    if (!linkSend(pkt, sizeof(PktJpegHead) + n)) return;  // radio swamped: skip the rest of this picture
  }
}

// Shown on the wireless screen while there is a problem.
static void sendStatus() {
  PktStatus s = {};
  s.h = {LINK_MAGIC, LINK_VERSION, PKT_STATUS, 0};
  if (!sensorOk) {
    snprintf(s.text, sizeof(s.text),
             "Thermal sensor not found.\nSDA -> GPIO%d, SCL -> GPIO%d\nVCC -> 3V3, GND -> GND%s", THERMAL_SDA,
             THERMAL_SCL, cameraOk ? "" : "\nColor camera failed too.");
  } else {
    snprintf(s.text, sizeof(s.text), "Color camera failed to\nstart (error 0x%x).\nThermal only.", (unsigned)cameraError);
  }
  linkSend(&s, offsetof(PktStatus, text) + strlen(s.text) + 1);
}

// ============================================================================
//  Standby and off
// ============================================================================

// Camera powered down, HaLow module held in reset, sensor bus idle; the
// USER key always wakes the head.
static void prepareSleep() {
  if (sensorTaskHandle) {
    sensorPause = true;
    for (const uint32_t t0 = millis(); !sensorPaused && millis() - t0 < 300;) delay(5);
  }
  if (cameraOk) esp_camera_deinit();
  pinMode(CAM_PWDN, OUTPUT);
  digitalWrite(CAM_PWDN, HIGH);
  gpio_hold_en((gpio_num_t)CAM_PWDN);
  pinMode(HALOW_RESET_N, OUTPUT);
  digitalWrite(HALOW_RESET_N, LOW);
  gpio_hold_en((gpio_num_t)HALOW_RESET_N);
  gpio_deep_sleep_hold_en();
  esp_sleep_enable_ext0_wakeup((gpio_num_t)USER_KEY, 0);
  rtc_gpio_pullup_en((gpio_num_t)USER_KEY);
  rtc_gpio_pulldown_dis((gpio_num_t)USER_KEY);
  Serial.flush();
}

static void deepSleepStandby() {
  esp_sleep_enable_timer_wakeup(STANDBY_CHECK_SECONDS * 1000000ULL);
  prepareSleep();
  esp_deep_sleep_start();
}

static void switchOff() {
  Serial.println("Switching off (USER key turns the head on)");
  rtcStandby = false;
  prepareSleep();
  esp_deep_sleep_start();
}

static void enterStandby() {
  Serial.println("No screen: standby");
  rtcStandby = true;
  rtcStandbyChecks = 0;
  deepSleepStandby();
}

// First thing after waking: in standby, listen briefly for a switched-on
// screen and go back to sleep if there is none. Returns to start up.
static void standbyCheck() {
  gpio_hold_dis((gpio_num_t)CAM_PWDN);
  gpio_hold_dis((gpio_num_t)HALOW_RESET_N);
  gpio_deep_sleep_hold_dis();
  pinMode(HALOW_RESET_N, OUTPUT);
  digitalWrite(HALOW_RESET_N, LOW);
  if (!rtcStandby) return;
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) {  // USER key
    rtcStandby = false;
    return;
  }
  pinMode(CAM_PWDN, OUTPUT);
  digitalWrite(CAM_PWDN, HIGH);
  initLink();
  // A switched-on screen says hello twice a second. A screen in standby only
  // sends probes, which don't count, so two sleeping boards can't keep
  // waking each other.
  const uint32_t t0 = millis();
  while (linkReady && !lastHelloMs && millis() - t0 < 600) delay(5);
  if (lastHelloMs) {
    rtcStandby = false;
    return;
  }
  if (++rtcStandbyChecks * (uint32_t)STANDBY_CHECK_SECONDS >= STANDBY_MINUTES * 60UL) switchOff();
  deepSleepStandby();
}

// A press that turned the head on doesn't count: the key must be released
// once before a press switches it off.
static bool keyWasDown = true, keyArmed = false;
static uint32_t keyDownAt = 0;

static void pollUserKey() {
  const bool down = digitalRead(USER_KEY) == LOW;
  const bool released = !down && keyWasDown && keyArmed && millis() - keyDownAt >= 30;
  if (!down) keyArmed = true;
  if (down && !keyWasDown) keyDownAt = millis();
  keyWasDown = down;
  if (released) switchOff();
}

// ============================================================================

void setup() {
  Serial.begin(115200);
  standbyCheck();  // may go back to sleep instead of returning
  pinMode(USER_KEY, INPUT_PULLUP);
  Serial.println("Thermal + color camera head (HT-HC33)");
  Serial.printf("PSRAM: %u bytes\n", (unsigned)ESP.getPsramSize());

  sensorOk = startSensor();
  cameraOk = startCamera();
  initLink();
  if (sensorOk) xTaskCreatePinnedToCore(sensorTask, "sensor", 8192, nullptr, 2, &sensorTaskHandle, 0);
  screenSeenMs = millis();
}

void loop() {
  static float temps[SENSOR_PIXELS];
  static uint32_t colorAt = 0, statusAt = 0;
  pollUserKey();

  const bool listening = screenListening();
  if (listening) screenSeenMs = millis();
  if (CAMERA_LINK_TIMEOUT_SECONDS > 0 && millis() - screenSeenMs > CAMERA_LINK_TIMEOUT_SECONDS * 1000UL) enterStandby();

  if (sensorOk && takeFrame(temps)) {
    processFrame(temps);
    if (haveFrame && (listening || probeReplyDue)) sendFrame();
    probeReplyDue = false;
  }

  const bool problem = !sensorOk || !cameraOk;
  if (problem && (listening || probeReplyDue) && millis() - statusAt >= 2000) {
    statusAt = millis();
    probeReplyDue = false;
    sendStatus();
  }

  if (cameraOk && listening && colorWanted() && millis() - colorAt >= 1000 / COLOR_MAX_FPS) {
    colorAt = millis();
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      if (fb->format == PIXFORMAT_JPEG) sendJpeg(fb->buf, fb->len);
      esp_camera_fb_return(fb);
    }
  }
  delay(1);
}
