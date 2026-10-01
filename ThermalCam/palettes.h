#pragma once
#include <stdint.h>

// Color palettes are defined as a few color stops and expanded at startup
// into 256-entry RGB565 lookup tables.

struct ColorStop {
  float pos;  // 0.0 (coldest) .. 1.0 (hottest)
  uint8_t r, g, b;
};

struct PaletteDef {
  const char *name;
  const ColorStop *stops;
  uint8_t count;
};

static const ColorStop IRONBOW[] = {
    {0.00f, 0, 0, 10},     {0.12f, 32, 0, 100},   {0.30f, 140, 0, 150},
    {0.50f, 220, 40, 60},  {0.66f, 250, 110, 0},  {0.84f, 255, 200, 20},
    {1.00f, 255, 255, 235},
};
static const ColorStop RAINBOW[] = {
    {0.00f, 0, 0, 120},  {0.15f, 0, 0, 255},   {0.35f, 0, 220, 255},
    {0.50f, 0, 220, 0},  {0.68f, 255, 240, 0}, {0.86f, 255, 0, 0},
    {1.00f, 255, 255, 255},
};
static const ColorStop HOT[] = {
    {0.00f, 0, 0, 0},
    {0.40f, 200, 0, 0},
    {0.75f, 255, 200, 0},
    {1.00f, 255, 255, 255},
};
static const ColorStop WHITE_HOT[] = {
    {0.00f, 0, 0, 0},
    {1.00f, 255, 255, 255},
};
static const ColorStop BLACK_HOT[] = {
    {0.00f, 255, 255, 255},
    {1.00f, 0, 0, 0},
};

#define PALETTE_DEF(name, stops) {name, stops, sizeof(stops) / sizeof(stops[0])}
static const PaletteDef PALETTES[] = {
    PALETTE_DEF("Ironbow", IRONBOW),
    PALETTE_DEF("Rainbow", RAINBOW),
    PALETTE_DEF("Hot", HOT),
    PALETTE_DEF("White hot", WHITE_HOT),
    PALETTE_DEF("Black hot", BLACK_HOT),
};
static const uint8_t PALETTE_COUNT = sizeof(PALETTES) / sizeof(PALETTES[0]);

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void buildPaletteLut(const PaletteDef &p, uint16_t lut[256]) {
  for (int i = 0; i < 256; i++) {
    float t = i / 255.0f;
    uint8_t s = 0;
    while (s < p.count - 2 && t > p.stops[s + 1].pos) s++;
    const ColorStop &a = p.stops[s];
    const ColorStop &b = p.stops[s + 1];
    float f = (t - a.pos) / (b.pos - a.pos);
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    lut[i] = rgb565(a.r + (b.r - a.r) * f, a.g + (b.g - a.g) * f, a.b + (b.b - a.b) * f);
  }
}
