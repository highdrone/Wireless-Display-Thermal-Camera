// Host test: camera -> wireless screen round trip.
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
std::vector<std::vector<uint8_t>> sentPackets;

#include "ThermalCam.cpp"
// Stand-ins used by the standby and radio shims.
void (*onSendHook)(const uint8_t *, size_t) = nullptr;
esp_sleep_wakeup_cause_t simWakeCause = ESP_SLEEP_WAKEUP_UNDEFINED;
uint64_t simTimerUs = 0, simExt1Mask = 0;

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
int main() {
  system("rm -rf sdcard");
  setup();
  battKnown = true; battPresent = true; battPercent = 72;
  printf("role: %s, link ready: %d\n", isCamera ? "camera" : "screen", linkReady);

  // No screen listening: the camera must not transmit.
  frames(20);
  printf("packets sent with no screen listening: %zu\n", sentPackets.size());

  // A screen says hello: the camera streams every frame.
  const uint8_t hello[] = {LINK_MAGIC, LINK_VERSION, PKT_HELLO, 0, 0};
  deliver(std::vector<uint8_t>(hello, hello + sizeof(hello)), 0x99);
  sentPackets.clear();
  frames(1);
  const size_t perFrame = sentPackets.size();
  size_t biggest = 0;
  for (auto &p : sentPackets) biggest = std::max(biggest, p.size());
  printf("packets per frame: %zu, largest %zu bytes (ESP-NOW limit 250)\n", perFrame, biggest);
  savePanel("tx_camera");
  const auto cameraScreen = panelCopy();
  const auto lastFrame = sentPackets;
  float camT[SENSOR_PIXELS]; memcpy(camT, smoothT, sizeof camT);
  const FrameStats camShown = shown; const uint8_t camPal = paletteIdx;

  // Become the screen, forget everything, and replay the packets.
  isCamera = false;
  memset(smoothT, 0, sizeof smoothT); shown = {NAN, NAN, NAN, -1, -1}; haveFrame = false;
  paletteIdx = (camPal + 2) % PALETTE_COUNT; rangeLo = rangeHi = NAN;
  sensorlessNote = "No thermal sensor on this board.\nI2C: 0x15 0x20 0x34 0x51 0x6B";
  fakeMillis += 5000; lastActivityMs = fakeMillis;
  loop(); savePanel("rx_waiting");
  for (auto &p : lastFrame) deliver(p, 0x01);
  fakeMillis += 20; loop();
  savePanel("rx_screen");
  float maxErr = 0;
  for (int i = 0; i < SENSOR_PIXELS; i++) maxErr = std::max(maxErr, fabsf(smoothT[i] - camT[i]));
  printf("screen got: palette %s, hot %d/%d, cold %d/%d, center %.2f/%.2f, max pixel error %.4f C\n",
         paletteIdx == camPal ? "same" : "DIFFERENT", shown.maxIdx, camShown.maxIdx, shown.minIdx, camShown.minIdx,
         shown.centerT, camShown.centerT, maxErr);
  const auto screen = panelCopy();
  int diff = 0, diffOutsideBadge = 0;
  for (int i = 0; i < LCD_WIDTH * LCD_HEIGHT; i++) {
    if (screen[i] == cameraScreen[i]) continue;
    diff++;
    const int nx = i % LCD_WIDTH, ny = i / LCD_WIDTH;  // panel coords; rotation 1: screen x = ny, y = 367 - nx
    const int sx = ny, sy = LCD_WIDTH - 1 - nx;
    if (!(sy < 40 && sx > 200)) diffOutsideBadge++;  // top-right: LIVE badge only on the camera
  }
  printf("screen vs camera: %d pixels differ, %d outside the top-right badge area\n", diff, diffOutsideBadge);

  // A second camera nearby is ignored while the first keeps sending.
  for (auto &p : lastFrame) deliver(p, 0x02);
  printf("second camera's frame accepted while first is active: %s\n", linkFrameReady ? "YES (bad)" : "no");

  // Camera goes quiet: back to the waiting page.
  fakeMillis += 2000; loop(); loop();
  savePanel("rx_lost");
  return 0;
}
