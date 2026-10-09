#pragma once
// Radio link between a thermal camera and wireless screens: ESP-NOW broadcast
// on WIRELESS_CHANNEL. A screen says hello twice a second; a camera streams
// only while it hears one.
//
// ThermalCam/link_protocol.h and ThermalCamHead/link_protocol.h must stay
// identical (tests/test_project.py checks this).
#include <stdint.h>

#define LINK_MAGIC 0x54  // 'T'
#define LINK_VERSION 1
#define LINK_SENSOR_PIXELS 768  // 32 x 24
#define LINK_CHUNK_PIXELS 112
#define LINK_CHUNKS ((LINK_SENSOR_PIXELS + LINK_CHUNK_PIXELS - 1) / LINK_CHUNK_PIXELS)

enum : uint8_t {
  PKT_HELLO = 1,   // screen -> camera
  PKT_META = 2,    // camera -> screen: readouts and settings for one frame
  PKT_PIXELS = 3,  // camera -> screen: part of one frame's temperatures
  PKT_JPEG = 4,    // color camera head -> screen: part of one color JPEG
  PKT_STATUS = 5,  // camera head -> screen: a problem to show (text)
};

struct __attribute__((packed)) PktHeader {
  uint8_t magic, version, type;
  uint16_t frame;  // frame number; for PKT_HELLO the HELLO_* bits below
};

// PKT_HELLO frame values. A probe comes from a screen in standby and is
// exactly HELLO_PROBE; a switched-on screen sends 0 or HELLO_WANT_COLOR.
#define HELLO_PROBE 1
#define HELLO_WANT_COLOR 2  // the screen shows a color view: please send PKT_JPEG

struct __attribute__((packed)) PktMeta {  // what the camera shows besides the pixels
  PktHeader h;
  uint8_t palette, flags;  // META_* bits
  int16_t hotIdx, coldIdx;
  float centerT, minT, maxT, rangeLo, rangeHi;
};
#define META_FAHRENHEIT 1
#define META_MARKERS 2
#define META_HEADLESS 4  // camera without a display: the screen picks palette and units

struct __attribute__((packed)) PktPixels {  // temperatures in 1/100 C, INT16_MIN = no reading
  PktHeader h;
  uint8_t chunk, count;
  int16_t centi[LINK_CHUNK_PIXELS];
};

// One piece of a color JPEG: `total` bytes in all, this piece starts at
// `offset`; the bytes follow the header. Pieces are up to 1400 bytes
// (ESP-NOW v2) or 220 bytes (v1).
struct __attribute__((packed)) PktJpegHead {
  PktHeader h;
  uint32_t total, offset;
};
#define LINK_JPEG_MAX (96 * 1024)

struct __attribute__((packed)) PktStatus {
  PktHeader h;
  char text[120];  // NUL-terminated
};
