// Host test: a wireless screen showing a thermal + color camera head. Plays
// the stream head_test recorded (head_stream.bin) to the screen firmware and
// checks the color pictures, the four views, the controls and alignment.
// Writes screenshots (fusion_*.ppm, screen orientation).
// Usage: fusion_test DATA_DIR [head_stream.bin]
#include <Arduino.h>
#include <Wire.h>
#include <random>
#include <vector>
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

// The canvas as the viewer sees it (landscape), as PPM.
static void saveScreen(const char *name) {
  char path[64];
  snprintf(path, sizeof path, "fusion_%s.ppm", name);
  FILE *f = fopen(path, "wb");
  fprintf(f, "P6\n%d %d\n255\n", scrW, scrH);
  const uint16_t *fb = gfx->getFramebuffer();
  for (int y = 0; y < scrH; y++)
    for (int x = 0; x < scrW; x++) {
      const uint16_t p = fb[fbBase + x * fbDx + y * fbDy];
      const uint8_t rgb[3] = {(uint8_t)((p >> 11) << 3), (uint8_t)(((p >> 5) & 0x3F) << 2), (uint8_t)((p & 0x1F) << 3)};
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
}

struct Rec { uint32_t t; std::vector<uint8_t> p; };
static std::vector<Rec> stream;
static size_t streamPos = 0;
static uint32_t streamBase = 0;  // fakeMillis at the stream's time 0 (it loops)

static void deliver(const std::vector<uint8_t> &pkt) {
  uint8_t mac[6] = {0x24, 0x6F, 0x28, 0x33, 0x00, 0x01};
  esp_now_recv_info_t info = {mac, nullptr, nullptr};
  onLinkReceive(&info, pkt.data(), pkt.size());
}

// Plays the head's stream for `ms` (looping it), running the screen as it goes.
static bool playing = true;
static void play(uint32_t ms) {
  for (const uint32_t t0 = fakeMillis; fakeMillis - t0 < ms;) {
    if (playing) {
      while (streamPos < stream.size() && streamBase + stream[streamPos].t <= fakeMillis) {
        deliver(stream[streamPos++].p);
        colorDecodeOnce();  // the decoder task, run in line
      }
      if (streamPos >= stream.size()) {
        streamPos = 0;
        streamBase = fakeMillis + 4;
      }
    }
    loop();
    fakeMillis += 4;
  }
}

static uint16_t lastHelloBits() {
  for (auto it = sentPackets.rbegin(); it != sentPackets.rend(); ++it)
    if (it->size() >= 5 && (*it)[2] == PKT_HELLO) return ((const PktHeader *)it->data())->frame;
  return 0xFFFF;
}

int main(int argc, char **argv) {
  const std::string data = argc > 1 ? argv[1] : "data";
  const std::string streamFile = argc > 2 ? argv[2] : "head_stream.bin";
  // The color picture the head streams, and what it decodes to.
  SimJpeg j;
  FILE *f = fopen((data + "/fusion_scene.jpg").c_str(), "rb");
  if (!f) { printf("no fusion_scene.jpg (bad)\n"); return 1; }
  uint8_t buf[65536];
  j.jpeg.assign(buf, buf + fread(buf, 1, sizeof buf, f));
  fclose(f);
  f = fopen((data + "/fusion_scene.rgb565").c_str(), "rb");
  j.rgb.resize(320 * 240);
  if (!f || fread(j.rgb.data(), 2, j.rgb.size(), f) != j.rgb.size()) { printf("no fusion_scene.rgb565 (bad)\n"); return 1; }
  fclose(f);
  j.w = 320;
  j.h = 240;
  simJpegs.push_back(j);
  f = fopen(streamFile.c_str(), "rb");
  if (!f) { printf("no %s: run head_test first (bad)\n", streamFile.c_str()); return 1; }
  for (;;) {
    Rec r;
    uint16_t len;
    if (fread(&r.t, 4, 1, f) != 1 || fread(&len, 2, 1, f) != 1) break;
    r.p.resize(len);
    if (fread(r.p.data(), 1, len, f) != len) break;
    stream.push_back(r);
  }
  fclose(f);

  system("rm -rf sdcard");
  setup();
  isCamera = false;  // this board is the wireless screen
  colorBegin();
  mode = MODE_CAMERA;
  screenDirty = true;
  streamBase = fakeMillis;

  // 1. The head's stream arrives.
  play(2500);
  const ColorFrame *cf = colorCurrent();
  printf("from the head: headless=%d, pictures decoded=%u, decode errors=%u, color picture %dx%d, screen asks for color: %s\n",
         linkHeadless, (unsigned)simJpegDecodes, (unsigned)colorDecodeErrors, cf ? cf->w : 0, cf ? cf->h : 0,
         (lastHelloBits() & HELLO_WANT_COLOR) ? "yes" : "NO (bad)");

  // 2. The four views; swiping up steps through them.
  for (int v = 0; v < VIEW_COUNT; v++) {
    viewMode = v;
    play(300);
    saveScreen(VIEW_NAMES[v]);
  }
  viewMode = VIEW_THERMAL;
  play(1100);
  printf("Thermal view: screen asks for color: %s\n", (lastHelloBits() & HELLO_WANT_COLOR) ? "YES (bad)" : "no");
  std::string seen;
  for (int i = 0; i < VIEW_COUNT; i++) {
    gestureStartMs = fakeMillis;
    gesture = GESTURE_SWIPE_UP;
    play(100);
    seen += std::string(toastText) + (i < VIEW_COUNT - 1 ? " > " : "");
  }
  printf("swipe up 4 times: %s (view %s)\n", seen.c_str(), VIEW_NAMES[viewMode]);

  // 3. BOOT on the screen changes the palette (the head has no buttons for it).
  const uint8_t pal = paletteIdx;
  buttonLevel = LOW; play(60); buttonLevel = HIGH; play(60);
  printf("BOOT with a camera head: palette %s -> %s (toast \"%s\")\n", PALETTES[pal].name, PALETTES[paletteIdx].name, toastText);
  paletteIdx = pal;

  // 4. Aligning: hold, drag, zoom, flip, tap to save.
  viewMode = VIEW_EDGES;
  touchStartX = touchX = 224;
  touchStartY = touchY = 168;
  touchDownMs = fakeMillis;
  touchDown = true;
  play(1300);
  printf("hold 1.3 s: aligning=%d (toast \"%s\")\n", aligning, toastText);
  touchDown = false;
  gestureStartMs = touchDownMs;
  gesture = GESTURE_OTHER;
  play(100);
  saveScreen("align");
  const float u0 = alignCu, v0 = alignCv, w0 = alignW;
  touchDownMs = fakeMillis;
  touchStartX = touchX = 200;
  touchStartY = touchY = 150;
  touchDown = true;
  play(40);
  for (int i = 0; i < 4; i++) { touchX += 10; touchY += 5; play(40); }
  touchDown = false;
  gestureStartMs = touchDownMs;
  gesture = GESTURE_SWIPE_RIGHT;  // a drag ends like a swipe: ignored while aligning
  play(100);
  printf("drag 40 px right, 20 px down: center %.3f,%.3f -> %.3f,%.3f (expect -%.3f, -%.3f), still aligning=%d, viewer=%d\n", u0,
         v0, alignCu, alignCv, 40 * w0 / imgW, 20 * alignRowShare(*colorCurrent()) / imgH, aligning, mode == MODE_VIEWER);
  buttonLevel = LOW; play(60); buttonLevel = HIGH; play(60);
  printf("BOOT: %s, width %.3f -> %.3f\n", toastText, w0, alignW);
  powerKeyPressed = true;
  play(60);
  printf("PWR: %s (thermal mirrored=%d)\n", toastText, viewMirror != MIRROR_IMAGE);
  powerKeyPressed = true;
  play(60);
  powerKeyPressed = true;
  play(60);
  powerKeyPressed = true;
  play(60);
  gestureStartMs = fakeMillis;
  gesture = GESTURE_TAP;
  play(60);
  printf("tap: aligning=%d, toast \"%s\", saved center %.3f,%.3f width %.3f flip %d\n", aligning, toastText,
         prefs.floats["alignU"], prefs.floats["alignV"], prefs.floats["alignW"], alignFlip);
  saveScreen("aligned");

  // 5. Color pictures stop (thermal still arrives): back to the thermal picture.
  for (auto &r : stream)
    if (r.p[2] == PKT_JPEG) r.p[2] = 0xEE;  // drop the pictures from now on
  viewMode = VIEW_BLEND;
  play(2600);
  saveScreen("color_stale");
  printf("pictures stopped for 2.6 s: color picture still used=%s\n", colorCurrent() && millis() - colorCurrent()->ms <= 2000 ? "YES (bad)" : "no");

  // 6. The head reports a problem (no sensor): the screen shows it.
  std::vector<uint8_t> st(sizeof(PktStatus));
  PktStatus *s = (PktStatus *)st.data();
  s->h = {LINK_MAGIC, LINK_VERSION, PKT_STATUS, 0};
  snprintf(s->text, sizeof s->text, "Thermal sensor not found.\nSDA -> GPIO11, SCL -> GPIO10\nVCC -> 3V3, GND -> GND");
  playing = false;
  for (int i = 0; i < 3; i++) { deliver(st); play(1000); }
  saveScreen("head_status");
  printf("head problem: waiting page shows it=%s\n", waitingShown ? "yes" : "NO (bad)");
  return 0;
}
