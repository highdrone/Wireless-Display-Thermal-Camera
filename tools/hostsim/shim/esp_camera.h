#pragma once
// Host stand-in for the esp32-camera driver: frames come from a test.
#include <stdint.h>
#include <stddef.h>
#include <vector>
#include "esp_now.h"
typedef int esp_err_t;
typedef enum { PIXFORMAT_RGB565, PIXFORMAT_JPEG = 4 } pixformat_t;
typedef enum { FRAMESIZE_QVGA = 5, FRAMESIZE_VGA = 8 } framesize_t;
typedef enum { CAMERA_FB_IN_PSRAM, CAMERA_FB_IN_DRAM } camera_fb_location_t;
typedef enum { CAMERA_GRAB_WHEN_EMPTY, CAMERA_GRAB_LATEST } camera_grab_mode_t;
enum { LEDC_TIMER_0 = 0, LEDC_CHANNEL_0 = 0 };
typedef int ledc_timer_t;
typedef int ledc_channel_t;
typedef struct {
  int pin_pwdn, pin_reset, pin_xclk, pin_sccb_sda, pin_sccb_scl;
  int pin_d7, pin_d6, pin_d5, pin_d4, pin_d3, pin_d2, pin_d1, pin_d0, pin_vsync, pin_href, pin_pclk;
  int xclk_freq_hz;
  ledc_timer_t ledc_timer;
  ledc_channel_t ledc_channel;
  pixformat_t pixel_format;
  framesize_t frame_size;
  int jpeg_quality;
  size_t fb_count;
  camera_fb_location_t fb_location;
  camera_grab_mode_t grab_mode;
  int sccb_i2c_port;
  size_t jpeg_buffer_size;
} camera_config_t;
typedef struct { uint8_t *buf; size_t len, width, height; pixformat_t format; } camera_fb_t;
struct sensor_id_t { uint16_t PID; };
struct sensor_t {
  sensor_id_t id;
  int (*set_vflip)(sensor_t *, int);
  int (*set_hmirror)(sensor_t *, int);
};
inline esp_err_t simCameraInitResult = 0;
inline int simCameraVflip = -1, simCameraHmirror = -1;
inline camera_config_t simCameraConfig;
inline std::vector<uint8_t> simCameraJpeg;
inline camera_fb_t simCameraFb;
inline bool simCameraFbOut = false;
inline esp_err_t esp_camera_init(const camera_config_t *c) { simCameraConfig = *c; return simCameraInitResult; }
inline esp_err_t esp_camera_deinit() { return 0; }
inline sensor_t *esp_camera_sensor_get() {
  static sensor_t s = {{0x3660}, [](sensor_t *, int v) { simCameraVflip = v; return 0; },
                       [](sensor_t *, int v) { simCameraHmirror = v; return 0; }};
  return &s;
}
inline camera_fb_t *esp_camera_fb_get() {
  if (simCameraJpeg.empty() || simCameraFbOut) return nullptr;
  simCameraFb = {simCameraJpeg.data(), simCameraJpeg.size(), 320, 240, PIXFORMAT_JPEG};
  simCameraFbOut = true;
  return &simCameraFb;
}
inline void esp_camera_fb_return(camera_fb_t *) { simCameraFbOut = false; }
