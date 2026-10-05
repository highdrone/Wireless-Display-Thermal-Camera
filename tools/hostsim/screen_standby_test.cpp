// Host test: wireless-screen standby (probe hellos, wake-ups, 30-minute cut-off).
#include <Arduino.h>
#include <Wire.h>
#include <random>
static uint32_t fakeMillis = 0;
uint32_t millis() { return fakeMillis; }
void delay(uint32_t ms) { fakeMillis += ms; }
static int buttonLevel = HIGH;
int digitalRead(int) { return buttonLevel; }
void pinMode(int, int) {}
void digitalWrite(int, int) {}
HostSerial Serial;
TwoWire Wire, Wire1;
#include <SD_MMC.h>
SDMMCFS SD_MMC;
#include <esp_now.h>
#include <esp_sleep.h>
std::vector<std::vector<uint8_t>> sentPackets;
void (*onSendHook)(const uint8_t *, size_t) = nullptr;
esp_sleep_wakeup_cause_t simWakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
uint64_t simTimerUs = 0, simExt1Mask = 0;

#include "ThermalCam.cpp"

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
static std::normal_distribution<float> noise(0.0f, 0.25f);  // MLX90640-like pixel noise at 16 Hz
static int mugShift = 0;
static void makeScene(float *t) {
  for (int r = 0; r < 24; r++)
    for (int c = 0; c < 32; c++) {
      float v = 21.0f + 0.04f * r;
      float dx = (c - 11.0f) / 3.2f, dy = (r - 8.0f) / 4.0f;
      if (dx * dx + dy * dy < 1.0f) v = 34.5f - 2.0f * (dx * dx + dy * dy);
      if (r >= 13 && fabsf(c - 11.0f) < 7.5f - (r - 13) * 0.1f) v = 30.0f + 0.5f * sinf(c);
      if (c >= 22 - mugShift && c <= 26 - mugShift && r >= 13 && r <= 19) v = 58.0f - 3.0f * fabsf(c - 24.0f + mugShift);
      if (c >= 25 && r <= 6) v = 15.5f;
      if (c >= 1 && c <= 3 && r >= 19 && r <= 21) v = 9.0f;  // a cold drink can, bottom-left of the sensor
      t[r * 32 + c] = v + noise(rng);
    }
}
static void savePanel(const char *name) {
  uint16_t *fb = ((Arduino_Canvas *)panel)->getFramebuffer();
  char path[64];
  snprintf(path, sizeof path, "v5_%s.ppm", name);
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6\n%d %d\n255\n", LCD_WIDTH, LCD_HEIGHT);
  for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
    uint16_t p = fb[i];
    uint8_t rgb[3] = {(uint8_t)((p >> 11) << 3), (uint8_t)(((p >> 5) & 0x3F) << 2), (uint8_t)((p & 0x1F) << 3)};
    fwrite(rgb, 1, 3, f);
  }
  fclose(f);
  printf("saved %-16s mode=%s pictures=%d viewIdx=%d note='%s'\n", path, mode == MODE_VIEWER ? "viewer" : "camera",
         (int)pictures.size(), viewIdx, viewerHasImage ? "" : viewerNote);
}
static void frames(int n) {
  for (int i = 0; i < n; i++) {
    makeScene(sharedFrame);
    sharedFrameNew = true;
    fakeMillis += 62;  // 16 Hz
    loop();
  }
}
static void swipe(uint8_t g, int16_t x = 200) {
  gestureStartMs = fakeMillis;
  gestureX = x;
  gesture = g;
  fakeMillis += 300;
  loop();
}
static void bootPress() {
  buttonLevel = LOW; fakeMillis += 20; loop();
  fakeMillis += 120; loop();
  buttonLevel = HIGH; loop();
}


static void deliver(const std::vector<uint8_t> &pkt, uint8_t macLast) {
  uint8_t mac[6] = {0x24, 0x6F, 0x28, 0, 0, macLast};
  esp_now_recv_info_t info = {mac, nullptr, nullptr};
  onLinkReceive(&info, pkt.data(), pkt.size());
}
static std::vector<uint16_t> panelCopy() {
  uint16_t *fb = ((Arduino_Canvas *)panel)->getFramebuffer();
  return std::vector<uint16_t>(fb, fb + LCD_WIDTH * LCD_HEIGHT);
}

static std::vector<std::vector<uint8_t>> cameraReply;
static bool cameraOn = false;
static void fakeCamera(const uint8_t *d, size_t n) {  // a camera nearby answers a probe hello
  if (cameraOn && n >= sizeof(PktHeader) && d[2] == PKT_HELLO && ((const PktHeader *)d)->frame == HELLO_PROBE)
    for (auto &p : cameraReply) deliver(p, 0x01);
}
static const char *wake(esp_sleep_wakeup_cause_t cause) {
  simWakeCause = cause;
  linkReady = false; linkCameraHeardMs = 0; isCamera = true;  // like a fresh boot
  try { return standbyCheck() == WAKE_AS_SCREEN ? "starts up as a screen" : "starts normally"; }
  catch (DeepSleep &) { return "goes back to sleep"; }
}
int main() {
  system("rm -rf sdcard");
  setup();
  // --- Camera side: a probe hello gets one frame back but doesn't count as a viewer.
  const uint8_t probe[] = {LINK_MAGIC, LINK_VERSION, PKT_HELLO, HELLO_PROBE, 0};
  frames(10);
  sentPackets.clear();
  deliver(std::vector<uint8_t>(probe, probe + sizeof(probe)), 0x99);
  frames(1);
  printf("camera answers a probe with %zu packets; counts it as a viewer: %s\n", sentPackets.size(), screenListening() ? "YES (bad)" : "no");
  cameraReply = sentPackets;
  sentPackets.clear(); frames(3);
  printf("camera keeps sending after the probe: %s\n", sentPackets.empty() ? "no" : "YES (bad)");

  // --- Screen side, on battery: losing the camera leads to standby, not power-off.
  isCamera = false; haveFrame = false; screenDirty = true;
  pmuFound = true; battKnown = true; battPresent = true; vbusPresent = false; battPercent = 64;
  sensorlessNote = "No thermal sensor on this board.\nI2C: 0x15 0x20 0x34 0x51 0x6B";
  lastActivityMs = fakeMillis; lastLinkFrameMs = 0;
  try { for (int i = 0; i < 2000; i++) { fakeMillis += 50; loop(); } printf("screen never slept (bad)\n"); }
  catch (DeepSleep &) {
    printf("screen went into standby after %.0f s: standby=%d, wakes every %llu s, tap/BOOT wake pins 0x%llx\n",
           (fakeMillis - lastActivityMs) / 1000.0, rtcStandby, (unsigned long long)(simTimerUs / 1000000), (unsigned long long)simExt1Mask);
  }
  savePanel("standby_msg");

  // --- Standby wake-ups.
  cameraOn = false; onSendHook = fakeCamera;
  const char *r;
  r = wake(ESP_SLEEP_WAKEUP_TIMER); printf("timer wake, camera off:  %s (checks=%u, standby=%d)\n", r, rtcStandbyChecks, rtcStandby);
  r = wake(ESP_SLEEP_WAKEUP_TIMER); printf("timer wake, camera off:  %s (checks=%u, standby=%d)\n", r, rtcStandbyChecks, rtcStandby);
  cameraOn = true;
  r = wake(ESP_SLEEP_WAKEUP_TIMER); printf("timer wake, camera on:   %s (standby=%d)\n", r, rtcStandby);
  rtcStandby = true; cameraOn = false;
  r = wake(ESP_SLEEP_WAKEUP_EXT1); printf("tap or BOOT wake:        %s (standby=%d)\n", r, rtcStandby);
  rtcStandby = true; rtcStandbyChecks = STANDBY_MINUTES * 60 / STANDBY_CHECK_SECONDS - 1;
  r = wake(ESP_SLEEP_WAKEUP_TIMER); printf("timer wake after %d min: %s (standby=%d)\n", STANDBY_MINUTES, r, rtcStandby);
  rtcStandby = false;
  r = wake(ESP_SLEEP_WAKEUP_UNDEFINED); printf("normal power-on:         %s\n", r);
  return 0;
}
