#pragma once
// Host stand-in for esp_jpeg. It doesn't decode JPEG: a test registers a JPEG
// file together with its decoded RGB565 pixels, and "decoding" checks that the
// bytes arriving over the radio are exactly that file.
#include <stdint.h>
#include <string.h>
#include <vector>
#include "esp_now.h"
#define ESP_FAIL -1
typedef enum { JPEG_IMAGE_SCALE_0 = 0, JPEG_IMAGE_SCALE_1_2, JPEG_IMAGE_SCALE_1_4, JPEG_IMAGE_SCALE_1_8 } esp_jpeg_image_scale_t;
typedef enum { JPEG_IMAGE_FORMAT_RGB888 = 0, JPEG_IMAGE_FORMAT_RGB565 } esp_jpeg_image_format_t;
typedef struct {
  uint8_t *indata;
  uint32_t indata_size;
  uint8_t *outbuf;
  uint32_t outbuf_size;
  esp_jpeg_image_format_t out_format;
  esp_jpeg_image_scale_t out_scale;
  struct { uint8_t swap_color_bytes : 1; } flags;
  struct { void *working_buffer; size_t working_buffer_size; } advanced;
  struct { uint32_t read; } priv;
} esp_jpeg_image_cfg_t;
typedef struct { uint16_t width, height; size_t output_len; } esp_jpeg_image_output_t;

struct SimJpeg { std::vector<uint8_t> jpeg; std::vector<uint16_t> rgb; uint16_t w, h; };
inline std::vector<SimJpeg> simJpegs;
inline uint32_t simJpegDecodes = 0;
inline const SimJpeg *simFindJpeg(const esp_jpeg_image_cfg_t *cfg) {
  for (auto &j : simJpegs)
    if (j.jpeg.size() == cfg->indata_size && memcmp(j.jpeg.data(), cfg->indata, cfg->indata_size) == 0) return &j;
  return nullptr;
}
inline esp_err_t esp_jpeg_get_image_info(esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *img) {
  const SimJpeg *j = simFindJpeg(cfg);
  if (!j) return ESP_FAIL;
  img->width = j->w;
  img->height = j->h;
  img->output_len = j->w * j->h * 2;
  return ESP_OK;
}
inline esp_err_t esp_jpeg_decode(esp_jpeg_image_cfg_t *cfg, esp_jpeg_image_output_t *img) {
  const SimJpeg *j = simFindJpeg(cfg);
  if (!j || cfg->out_scale != JPEG_IMAGE_SCALE_0 || cfg->outbuf_size < j->rgb.size() * 2) return ESP_FAIL;
  memcpy(cfg->outbuf, j->rgb.data(), j->rgb.size() * 2);
  img->width = j->w;
  img->height = j->h;
  img->output_len = j->rgb.size() * 2;
  simJpegDecodes++;
  return ESP_OK;
}
