// Host test: the thermal + color camera head (ThermalCamHead on a Heltec
// HT-HC33): finding the sensor on its pins, status messages, streaming
// thermal frames and color pictures, standby and the USER key. It also writes
// what it streamed to head_stream.bin, which fusion_test plays to a screen.
// Usage: head_test DATA_DIR
#include <Arduino.h>
#include <Wire.h>
#include <random>
#include <vector>
static uint32_t fakeMillis = 0;
uint32_t millis() { return fakeMillis; }
static void (*delayHook)() = nullptr;
void delay(uint32_t ms) { fakeMillis += ms; if (delayHook) delayHook(); }
static int userKeyLevel = HIGH;
int digitalRead(int pin) { return pin == 0 ? userKeyLevel : HIGH; }
void pinMode(int, int) {}
void digitalWrite(int, int) {}
HostSerial Serial;
TwoWire Wire, Wire1;
#include <esp_now.h>
#include <esp_sleep.h>
std::vector<std::vector<uint8_t>> sentPackets;
void (*onSendHook)(const uint8_t *, size_t) = nullptr;
esp_sleep_wakeup_cause_t simWakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
uint64_t simTimerUs = 0, simExt1Mask = 0;

#include "ThermalCamHead.cpp"

// Stand-ins for the sensor driver (frames are injected directly).
void MLX90640_SetWire(TwoWire *) {}
extern "C" {
int MLX90640_DumpEE(uint8_t, uint16_t *) { return 0; }
int MLX90640_ExtractParameters(uint16_t *, paramsMLX90640 *) { return 0; }
int MLX90640_SetChessMode(uint8_t) { return 0; }
int MLX90640_SetResolution(uint8_t, uint8_t) { return 0; }
int MLX90640_SetRefreshRate(uint8_t, uint8_t) { return 0; }
int MLX90640_I2CRead(uint8_t, uint16_t, uint16_t n, uint16_t *d) { memset(d, 0, n * 2); return 0; }
int MLX90640_GetFrameData(uint8_t, uint16_t *) { return 0; }
float MLX90640_GetTa(uint16_t *, const paramsMLX90640 *) { return 25; }
void MLX90640_CalculateTo(uint16_t *, const paramsMLX90640 *, float, float, float *) {}
}

static std::mt19937 rng(1);
static std::normal_distribution<float> noise(0.0f, 0.25f);
static void makeScene(float *t) {  // the same made-up scene as the other tests
  for (int r = 0; r < 24; r++)
    for (int c = 0; c < 32; c++) {
      float v = 21.0f + 0.04f * r;
      float dx = (c - 11.0f) / 3.2f, dy = (r - 8.0f) / 4.0f;
      if (dx * dx + dy * dy < 1.0f) v = 34.5f - 2.0f * (dx * dx + dy * dy);
      if (r >= 13 && fabsf(c - 11.0f) < 7.5f - (r - 13) * 0.1f) v = 30.0f + 0.5f * sinf(c);
      if (c >= 22 && c <= 26 && r >= 13 && r <= 19) v = 58.0f - 3.0f * fabsf(c - 24.0f);
      if (c >= 25 && r <= 6) v = 15.5f;
      if (c >= 1 && c <= 3 && r >= 19 && r <= 21) v = 9.0f;
      t[r * 32 + c] = v + noise(rng);
    }
}

static const uint8_t SCREEN_MAC = 0x42;
static void hello(uint16_t bits) {
  const PktHeader h = {LINK_MAGIC, LINK_VERSION, PKT_HELLO, bits};
  uint8_t mac[6] = {0x24, 0x6F, 0x28, 0, 0, SCREEN_MAC};
  esp_now_recv_info_t info = {mac, nullptr, nullptr};
  onLinkReceive(&info, (const uint8_t *)&h, sizeof(h));
}

// Runs the head for `ms` with a 16 Hz sensor and, if `bits` >= 0, a screen
// saying hello twice a second.
static void run(uint32_t ms, int bits) {
  static uint32_t helloAt = 0, frameAt = 0;
  for (const uint32_t t0 = fakeMillis; fakeMillis - t0 < ms;) {
    if (bits >= 0 && fakeMillis - helloAt >= 500) { helloAt = fakeMillis; hello(bits); }
    if (fakeMillis - frameAt >= 62) { frameAt = fakeMillis; makeScene(sharedFrame); sharedFrameNew = true; }
    loop();
    fakeMillis += 4;
  }
}

static int count(uint8_t type) {
  int n = 0;
  for (auto &p : sentPackets) n += p.size() >= 3 && p[2] == type;
  return n;
}

static const char *listen(esp_sleep_wakeup_cause_t cause, int bits) {
  static int helloBits = -1;
  helloBits = bits;
  simWakeCause = cause;
  linkReady = false;
  lastHelloMs = 0;
  delayHook = [] { if (helloBits >= 0) hello(helloBits); };
  const char *r = "starts up";
  try { standbyCheck(); } catch (DeepSleep &) { r = rtcStandby ? "goes back to sleep" : "switches off"; }
  delayHook = nullptr;
  return r;
}

int main(int argc, char **argv) {
  const std::string data = argc > 1 ? argv[1] : "data";
  FILE *f = fopen((data + "/fusion_scene.jpg").c_str(), "rb");
  if (!f) { printf("no %s/fusion_scene.jpg (bad)\n", data.c_str()); return 1; }
  uint8_t buf[65536];
  simCameraJpeg.assign(buf, buf + fread(buf, 1, sizeof buf, f));
  fclose(f);

  // 1. No sensor connected: the head says so on the screen.
  setup();
  sentPackets.clear();
  run(2500, 0);
  std::string status;
  for (auto &p : sentPackets)
    if (p[2] == PKT_STATUS) status = std::string((const char *)p.data() + offsetof(PktStatus, text));
  printf("no sensor: %d status message(s), first line \"%s\", thermal frames %d %s\n", count(PKT_STATUS),
         status.substr(0, status.find('\n')).c_str(), count(PKT_META), count(PKT_META) ? "(bad)" : "");

  // 2. Sensor soldered to the other SD pins, SDA and SCL swapped: still found.
  TwoWire::simSensorSda = THERMAL_ALT_SCL;
  TwoWire::simSensorScl = THERMAL_ALT_SDA;
  sensorOk = startSensor();
  printf("sensor on GPIO%d/GPIO%d (swapped alternate pins): found=%d, using SDA=GPIO%d SCL=GPIO%d\n", THERMAL_ALT_SCL,
         THERMAL_ALT_SDA, sensorOk, sensorSda, sensorScl);
  TwoWire::simSensorSda = THERMAL_SDA;
  TwoWire::simSensorScl = THERMAL_SCL;
  sensorOk = startSensor();
  printf("sensor on the main pins: found=%d SDA=GPIO%d SCL=GPIO%d; camera vflip=%d hmirror=%d, %s %dx%d-class JPEG\n",
         sensorOk, sensorSda, sensorScl, simCameraVflip, simCameraHmirror,
         simCameraConfig.pixel_format == PIXFORMAT_JPEG ? "streams" : "NOT (bad)", 320, 240);

  // 3. A screen in Thermal view: thermal frames only.
  sentPackets.clear();
  run(2000, 0);
  printf("screen in Thermal view, 2 s: %d frames, %d picture packets %s\n", count(PKT_META), count(PKT_JPEG),
         count(PKT_JPEG) ? "(bad)" : "");

  // 4. A screen in a color view: color pictures too. Recorded for fusion_test.
  sentPackets.clear();
  std::vector<uint32_t> times;
  onSendHook = [](const uint8_t *, size_t) {};
  const uint32_t streamStart = fakeMillis;
  size_t before = 0;
  std::vector<std::pair<uint32_t, std::vector<uint8_t>>> stream;
  for (const uint32_t t0 = fakeMillis; fakeMillis - t0 < 3000;) {
    run(4, HELLO_WANT_COLOR);
    for (; before < sentPackets.size(); before++) stream.push_back({fakeMillis - streamStart, sentPackets[before]});
  }
  int pictures = 0, headless = 0, biggest = 0;
  for (auto &p : sentPackets) {
    if (p[2] == PKT_JPEG && ((const PktJpegHead *)p.data())->offset == 0) pictures++;
    if (p[2] == PKT_META && (((const PktMeta *)p.data())->flags & META_HEADLESS)) headless++;
    biggest = std::max<int>(biggest, p.size());
  }
  printf("screen in a color view, 3 s: %d thermal frames (all marked headless: %s), %d color pictures, largest packet %d bytes\n",
         count(PKT_META), headless == count(PKT_META) ? "yes" : "NO (bad)", pictures, biggest);
  FILE *out = fopen("head_stream.bin", "wb");
  for (auto &[t, p] : stream) {
    const uint16_t len = p.size();
    fwrite(&t, 4, 1, out);
    fwrite(&len, 2, 1, out);
    fwrite(p.data(), 1, len, out);
  }
  fclose(out);
  printf("recorded %zu packets to head_stream.bin\n", stream.size());

  // 5. A screen in standby probes: one thermal frame back, no pictures.
  fakeMillis += 5000;  // the screen's hellos have stopped
  sentPackets.clear();
  hello(HELLO_PROBE);
  run(200, -1);
  printf("probe from a sleeping screen: %d frame(s), %d picture packets\n", count(PKT_META), count(PKT_JPEG));

  // 6. No screen: standby, and the wake-ups.
  uint32_t slept = 0;
  try { run(60000, -1); } catch (DeepSleep &) { slept = 1; }
  printf("no screen: standby=%d (rtcStandby=%d), wakes every %llu s or on GPIO%d\n", slept, rtcStandby,
         (unsigned long long)(simTimerUs / 1000000), simExt0Pin);
  printf("timer wake, nothing heard:      %s\n", listen(ESP_SLEEP_WAKEUP_TIMER, -1));
  printf("timer wake, only screen probes: %s\n", listen(ESP_SLEEP_WAKEUP_TIMER, HELLO_PROBE));
  const char *r = listen(ESP_SLEEP_WAKEUP_TIMER, 0);
  printf("timer wake, screen switched on: %s (rtcStandby=%d)\n", r, rtcStandby);
  rtcStandby = true;
  rtcStandbyChecks = STANDBY_MINUTES * 60 / STANDBY_CHECK_SECONDS - 1;
  printf("after %d min in standby:        %s\n", STANDBY_MINUTES, listen(ESP_SLEEP_WAKEUP_TIMER, -1));
  rtcStandby = false;
  printf("USER key wake from off:         %s\n", listen(ESP_SLEEP_WAKEUP_EXT0, -1));

  // 7. USER key: the press that turned it on doesn't count; the next one switches off.
  keyWasDown = true;  // a fresh start after waking, with the key still held, as setup() leaves it
  keyArmed = false;
  linkReady = true;
  screenSeenMs = fakeMillis;
  userKeyLevel = LOW;
  bool off = false;
  try { run(300, 0); userKeyLevel = HIGH; run(300, 0); } catch (DeepSleep &) { off = true; }
  printf("release of the key that woke it: switched off=%s\n", off ? "YES (bad)" : "no");
  rtcStandby = true;  // so we can see switchOff() clear it
  try { userKeyLevel = LOW; run(100, 0); userKeyLevel = HIGH; run(100, 0); } catch (DeepSleep &) { off = true; }
  const bool standbyAfter = rtcStandby;
  printf("next press: switched off=%s, off rather than standby=%s\n", off ? "yes" : "NO (bad)", standbyAfter ? "NO (bad)" : "yes");
  return 0;
}
