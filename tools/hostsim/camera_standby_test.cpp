// Host test: the camera with a wireless screen: its display stays dark while the
// screen watches, and it goes into standby when the screen goes away.
#include <Arduino.h>
#include <Wire.h>
#include <random>
static uint32_t fakeMillis = 0;
uint32_t millis() { return fakeMillis; }
static void (*delayHook)() = nullptr;
void delay(uint32_t ms) { fakeMillis += ms; if (delayHook) delayHook(); }
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
static const uint8_t HELLO[] = {LINK_MAGIC, LINK_VERSION, PKT_HELLO, 0, 0};
static const uint8_t PROBE[] = {LINK_MAGIC, LINK_VERSION, PKT_HELLO, HELLO_PROBE, 0};
static void sendToCamera(const uint8_t *p) { deliver(std::vector<uint8_t>(p, p + 5), 0x42); }
static void onBattery() { pmuFound = true; battKnown = true; battPresent = true; vbusPresent = false; battPercent = 64; }

// Camera running with frames; a screen says hello every 500 ms while `screenOn`.
static bool screenOn = false;
static uint32_t helloAt = 0, firstWarnAt = 0;
static char warnText[24] = "";
static void camStep() {
  if (screenOn && fakeMillis - helloAt >= 500) { helloAt = fakeMillis; sendToCamera(HELLO); }
  makeScene(sharedFrame); sharedFrameNew = true;
  fakeMillis += 62;
  loop();
  if (idleWarnLeftMs && !firstWarnAt) {
    firstWarnAt = fakeMillis;
    snprintf(warnText, sizeof warnText, "%s", screenListening() ? "Screen off" : willStandby() ? "Standby" : willPowerOff() ? "Turning off" : "Screen off");  // as drawIdleWarning
  }
}
// Runs the camera until it deep-sleeps or `maxMs` passes; returns ms run, or 0 if it never slept.
static uint32_t runUntilSleep(uint32_t maxMs) {
  const uint32_t t0 = fakeMillis;
  try { while (fakeMillis - t0 < maxMs) camStep(); } catch (DeepSleep &) { return fakeMillis - t0; }
  return 0;
}
// During a standby check, the "screen" sends what this says.
static const uint8_t *listenSends = nullptr;
static uint32_t listenEvery = 0, listenLast = 0;
static void listenHook() {
  if (listenSends && fakeMillis - listenLast >= listenEvery) { listenLast = fakeMillis; sendToCamera(listenSends); }
}
static const char *names[] = {"starts normally", "starts up as a screen", "starts up as the camera"};
static const char *wakeCamera(esp_sleep_wakeup_cause_t cause, const uint8_t *sends, uint32_t every) {
  simWakeCause = cause;
  linkReady = false; lastHelloMs = 0; linkCameraHeardMs = 0;  // like a fresh boot
  listenSends = sends; listenEvery = every; listenLast = fakeMillis; delayHook = listenHook;
  const char *r;
  try { r = names[standbyCheck()]; } catch (DeepSleep &) { r = "goes back to sleep"; }
  delayHook = nullptr;
  return r;
}
static void resetCamera() {  // back to a running camera that has never had a screen
  isCamera = true; linkReady = true; mode = MODE_CAMERA; screenAsleep = false; darkForLink = false; idleWarnLeftMs = 0;
  hadScreen = false; screenSeenMs = 0; lastHelloMs = 0; rtcStandby = false; screenOn = false; firstWarnAt = 0;
  lastActivityMs = fakeMillis;
}

int main() {
  system("rm -rf sdcard");
  onBattery();
  setup();
  printf("cold start: camera=%d link=%d hadScreen=%d\n", isCamera, linkReady, hadScreen);
  resetCamera();

  // 1. Screen connected: no auto-off at all, and the camera's own display goes dark.
  screenOn = true; sentPackets.clear();
  camStep(); camStep();
  printf("screen connects: camera display %s\n", screenAsleep ? "off" : "ON (bad)");
  uint32_t slept = runUntilSleep(5 * 60 * 1000);
  printf("battery, screen connected 5 min: auto-off=%s countdown=%s display=%s streamed=%zu packets\n", slept ? "YES (bad)" : "never",
         firstWarnAt ? "YES (bad)" : "never", screenAsleep ? "off" : "ON (bad)", sentPackets.size());

  // 1b. A tap turns it on; it goes dark again after a minute without use, with a countdown.
  touchSeen = true; const uint32_t tapAt = fakeMillis; camStep();
  printf("tap while connected: display %s\n", screenAsleep ? "OFF (bad)" : "on");
  firstWarnAt = 0;
  while (!screenAsleep && fakeMillis - tapAt < 120000) camStep();
  printf("then idle: countdown \"%s\" at %.1f s, dark again at %.1f s (deep sleep: never)\n", warnText, (firstWarnAt - tapAt) / 1000.0,
         (fakeMillis - tapAt) / 1000.0);

  // 1c. BOOT wakes it too, without changing the palette.
  const uint8_t pal = paletteIdx;
  bootPress(); camStep();
  printf("BOOT while connected: display %s, palette %s\n", screenAsleep ? "OFF (bad)" : "on", paletteIdx == pal ? "unchanged" : "CHANGED (bad)");
  while (!screenAsleep) camStep();

  // 2. Screen switched off while the camera's display is dark: standby, without lighting up.
  screenOn = false; firstWarnAt = 0;
  const uint32_t lastHello = helloAt;
  slept = runUntilSleep(5 * 60 * 1000);
  printf("screen gone: countdown shown=%s, standby at %.1f s after the last hello; rtcStandby=%d asCamera=%d wakes every %llu s\n",
         firstWarnAt ? "YES (bad: display is dark)" : "no", (fakeMillis - lastHello) / 1000.0, rtcStandby, rtcStandbyCamera,
         (unsigned long long)(simTimerUs / 1000000));
  savePanel("cam_standby_msg");

  // 3. Standby checks.
  { const char *r = wakeCamera(ESP_SLEEP_WAKEUP_TIMER, nullptr, 0); printf("timer wake, nothing heard:      %s (checks=%u)\n", r, rtcStandbyChecks); }
  { const char *r = wakeCamera(ESP_SLEEP_WAKEUP_TIMER, PROBE, 150); printf("timer wake, only screen probes: %s (checks=%u)\n", r, rtcStandbyChecks); }
  { const char *r = wakeCamera(ESP_SLEEP_WAKEUP_TIMER, HELLO, 500); printf("timer wake, screen switched on: %s (rtcStandby=%d)\n", r, rtcStandby); }
  rtcStandby = true; rtcStandbyCamera = true;
  printf("tap or BOOT wake:               %s\n", wakeCamera(ESP_SLEEP_WAKEUP_EXT1, nullptr, 0));
  rtcStandby = true; rtcStandbyCamera = true; rtcStandbyChecks = STANDBY_MINUTES * 60 / STANDBY_CHECK_SECONDS - 1;
  { const char *r = wakeCamera(ESP_SLEEP_WAKEUP_TIMER, nullptr, 0); printf("timer wake after %d min:        %s (rtcStandby=%d)\n", STANDBY_MINUTES, r, rtcStandby); }

  // 4. Full start-up after a screen woke it: streams straight away and stays armed.
  rtcStandby = true; rtcStandbyCamera = true; rtcStandbyChecks = 3;
  simWakeCause = ESP_SLEEP_WAKEUP_TIMER; linkReady = false; lastHelloMs = 0; hadScreen = false;
  listenSends = HELLO; listenEvery = 500; listenLast = fakeMillis; delayHook = listenHook;
  setup();
  delayHook = nullptr;
  screenOn = true; helloAt = fakeMillis; sentPackets.clear();
  for (int i = 0; i < 40; i++) camStep();
  printf("after wake-up by screen: camera=%d hadScreen=%d streaming=%s display=%s\n", isCamera, hadScreen,
         sentPackets.empty() ? "no (bad)" : "yes", screenAsleep ? "off" : "ON (bad)");

  // 5. Someone uses the camera after the screen goes: normal 60 s limit from that touch.
  screenOn = false; firstWarnAt = 0;
  for (int i = 0; i < 80; i++) camStep();  // 5 s
  touchSeen = true; const uint32_t touchAt = fakeMillis;
  slept = runUntilSleep(5 * 60 * 1000);
  printf("touched 5 s after the screen went: standby %.1f s after the touch (expect ~%d)\n", (fakeMillis - touchAt) / 1000.0, IDLE_OFF_SECONDS);

  // 6. USB power: the camera keeps running; its display stays dark while a screen watches.
  resetCamera(); vbusPresent = true; for (int i = 0; i < 3; i++) camStep();
  screenOn = true; for (int i = 0; i < 40; i++) camStep();
  printf("USB, screen connected: display %s\n", screenAsleep ? "off" : "ON (bad)");
  screenOn = false; firstWarnAt = 0;
  slept = runUntilSleep(60 * 1000);
  printf("USB, screen gone: deep sleep=%s, display %s\n", slept ? "YES (bad)" : "no", screenAsleep ? "off" : "ON (bad)");
  screenOn = true; helloAt = 0; camStep(); camStep();
  printf("USB, screen back on: display %s\n", screenAsleep ? "off (stays dark)" : "ON (bad)");
  touchSeen = true; camStep();
  printf("USB, tap: display %s\n", screenAsleep ? "OFF (bad)" : "on");

  // 7. Battery, never had a screen: plain auto-off, no standby.
  resetCamera(); onBattery(); for (int i = 0; i < 3; i++) camStep();
  resetCamera(); onBattery();
  const uint32_t t7 = fakeMillis;
  slept = runUntilSleep(90 * 1000);
  printf("battery, no screen ever: standby=%s, countdown \"%s\" at %.1f s, power-off requested=%d\n", slept ? "YES (bad)" : "no",
         warnText, (firstWarnAt - t7) / 1000.0, (int)powerOffRequested);
  return 0;
}
