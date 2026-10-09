#pragma once
// Wireless screen: color pictures from a thermal + color camera head
// (ThermalCamHead). JPEG pieces arrive on the Wi-Fi task and are put back
// together; a task on core 0 decodes each finished picture to RGB565 and
// works out an outline map for the Edges view. The main loop takes the
// newest decoded picture with colorCurrent().
//
// Buffers are triple-buffered both for the JPEG bytes and for the decoded
// pictures, so the radio, the decoder and the drawing never touch the same
// memory at the same time.
#include <Arduino.h>
#include <utility>
#include <jpeg_decoder.h>
#include "link_protocol.h"

#define COLOR_MAX_W 640
#define COLOR_MAX_H 480

struct ColorFrame {
  uint16_t *rgb;   // RGB565, w x h
  uint8_t *edges;  // outline strength 0..255, w x h
  uint16_t w, h;
  uint32_t ms;     // when it was decoded; 0 = never
};

static portMUX_TYPE colorLock = portMUX_INITIALIZER_UNLOCKED;
static uint8_t *colorJpeg[3];  // assembling / ready / decoding
static uint8_t jpegAsm = 0, jpegReady = 1, jpegDec = 2;
static uint32_t jpegReadyLen = 0;
static bool jpegReadyNew = false;
static uint16_t jpegAsmNo = 0;
static uint32_t jpegAsmTotal = 0, jpegAsmGot = 0;
static bool jpegAsmValid = false;

static ColorFrame colorFrames[3];  // front (drawing) / ready / back (decoding)
static uint8_t frameFront = 0, frameReady = 1, frameBack = 2;
static bool colorFrameNew = false;
static uint8_t *colorLuma = nullptr;  // scratch for the outline map
static TaskHandle_t colorTaskHandle = nullptr;
static volatile uint32_t colorDecodeErrors = 0;

// Wi-Fi task: one piece of a JPEG. Pieces of one picture arrive in order; a
// lost piece just drops that picture.
static void colorRx(const uint8_t *data, int len) {
  if (!colorJpeg[0] || len < (int)sizeof(PktJpegHead)) return;
  PktJpegHead h;
  memcpy(&h, data, sizeof(h));
  const uint32_t n = len - sizeof(PktJpegHead);
  if (h.total == 0 || h.total > LINK_JPEG_MAX || h.offset > h.total || n > h.total - h.offset) return;
  if (h.offset == 0) {  // a new picture starts
    jpegAsmNo = h.h.frame;
    jpegAsmTotal = h.total;
    jpegAsmGot = 0;
    jpegAsmValid = true;
  } else if (!jpegAsmValid || h.h.frame != jpegAsmNo || h.offset != jpegAsmGot || h.total != jpegAsmTotal) {
    jpegAsmValid = false;  // missed a piece: wait for the next picture
    return;
  }
  memcpy(colorJpeg[jpegAsm] + h.offset, data + sizeof(PktJpegHead), n);
  jpegAsmGot += n;
  if (jpegAsmGot < jpegAsmTotal) return;
  jpegAsmValid = false;
  portENTER_CRITICAL(&colorLock);
  std::swap(jpegAsm, jpegReady);
  jpegReadyLen = jpegAsmTotal;
  jpegReadyNew = true;
  portEXIT_CRITICAL(&colorLock);
  if (colorTaskHandle) xTaskNotifyGive(colorTaskHandle);
}

// Outline strength from the picture's brightness (Sobel), for the Edges view.
static void colorComputeEdges(ColorFrame &f) {
  const int w = f.w, h = f.h;
  for (int i = 0; i < w * h; i++) {
    const uint16_t c = f.rgb[i];
    const int r = (c >> 11) << 3, g = ((c >> 5) & 0x3F) << 2, b = (c & 0x1F) << 3;
    colorLuma[i] = (r * 77 + g * 150 + b * 29) >> 8;
  }
  memset(f.edges, 0, w * h);
  for (int y = 1; y < h - 1; y++) {
    const uint8_t *a = colorLuma + (y - 1) * w, *m = colorLuma + y * w, *z = colorLuma + (y + 1) * w;
    uint8_t *out = f.edges + y * w;
    for (int x = 1; x < w - 1; x++) {
      const int gx = (a[x + 1] + 2 * m[x + 1] + z[x + 1]) - (a[x - 1] + 2 * m[x - 1] + z[x - 1]);
      const int gy = (z[x - 1] + 2 * z[x] + z[x + 1]) - (a[x - 1] + 2 * a[x] + a[x + 1]);
      const int mag = abs(gx) + abs(gy);
      const int e = (mag - 64) * 3 / 4;  // ignore texture and noise, saturate on strong edges
      out[x] = e <= 0 ? 0 : e >= 255 ? 255 : e;
    }
  }
}

// Decodes the newest finished JPEG, if there is one.
static void colorDecodeOnce() {
  uint32_t len = 0;
  portENTER_CRITICAL(&colorLock);
  if (jpegReadyNew) {
    std::swap(jpegReady, jpegDec);
    len = jpegReadyLen;
    jpegReadyNew = false;
  }
  portEXIT_CRITICAL(&colorLock);
  if (!len) return;

  ColorFrame &back = colorFrames[frameBack];
  esp_jpeg_image_cfg_t cfg = {};
  cfg.indata = colorJpeg[jpegDec];
  cfg.indata_size = len;
  cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
  esp_jpeg_image_output_t info = {};
  if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) {
    colorDecodeErrors = colorDecodeErrors + 1;
    return;
  }
  // Shrink anything bigger than 640x480 while decoding.
  cfg.out_scale = info.width > 2 * COLOR_MAX_W ? JPEG_IMAGE_SCALE_1_4
                  : info.width > COLOR_MAX_W   ? JPEG_IMAGE_SCALE_1_2
                                               : JPEG_IMAGE_SCALE_0;
  cfg.outbuf = (uint8_t *)back.rgb;
  cfg.outbuf_size = COLOR_MAX_W * COLOR_MAX_H * 2;
  esp_jpeg_image_output_t out = {};
  if (esp_jpeg_decode(&cfg, &out) != ESP_OK || out.width < 3 || out.height < 3 || out.width > COLOR_MAX_W ||
      out.height > COLOR_MAX_H) {
    colorDecodeErrors = colorDecodeErrors + 1;
    return;
  }
  back.w = out.width;
  back.h = out.height;
  colorComputeEdges(back);
  back.ms = millis();
  if (!back.ms) back.ms = 1;
  portENTER_CRITICAL(&colorLock);
  std::swap(frameBack, frameReady);
  colorFrameNew = true;
  portEXIT_CRITICAL(&colorLock);
}

static void colorTask(void *) {
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    colorDecodeOnce();
  }
}

// Allocates the buffers (about 3 MB of PSRAM) and starts the decoder.
static bool colorBegin() {
  if (colorJpeg[0]) return true;
  for (int i = 0; i < 3; i++) {
    colorJpeg[i] = (uint8_t *)ps_malloc(LINK_JPEG_MAX);
    colorFrames[i].rgb = (uint16_t *)ps_malloc(COLOR_MAX_W * COLOR_MAX_H * 2);
    colorFrames[i].edges = (uint8_t *)ps_malloc(COLOR_MAX_W * COLOR_MAX_H);
    colorFrames[i].w = colorFrames[i].h = 0;
    colorFrames[i].ms = 0;
  }
  colorLuma = (uint8_t *)ps_malloc(COLOR_MAX_W * COLOR_MAX_H);
  bool ok = colorLuma != nullptr;
  for (int i = 0; i < 3; i++) ok = ok && colorJpeg[i] && colorFrames[i].rgb && colorFrames[i].edges;
  if (!ok) {
    Serial.println("Not enough PSRAM for color pictures");
    for (int i = 0; i < 3; i++) {
      free(colorJpeg[i]);
      free(colorFrames[i].rgb);
      free(colorFrames[i].edges);
      colorJpeg[i] = nullptr;
    }
    free(colorLuma);
    colorLuma = nullptr;
    return false;
  }
  xTaskCreatePinnedToCore(colorTask, "color", 8192, nullptr, 1, &colorTaskHandle, 0);
  return true;
}

// Main loop: the newest decoded picture, or nullptr before the first one.
static const ColorFrame *colorCurrent() {
  if (!colorJpeg[0]) return nullptr;
  portENTER_CRITICAL(&colorLock);
  if (colorFrameNew) {
    std::swap(frameFront, frameReady);
    colorFrameNew = false;
  }
  portEXIT_CRITICAL(&colorLock);
  const ColorFrame &f = colorFrames[frameFront];
  return f.ms ? &f : nullptr;
}
