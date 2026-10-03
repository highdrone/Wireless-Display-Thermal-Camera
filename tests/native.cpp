// Copyright (c) 2026 ESP32 Thermal Camera contributors. MIT License.
#include "../ThermalCam/bmp_header.h"
#include "../ThermalCam/palettes.h"
#include <cassert>
#include <cstring>
#include <limits>
#include <initializer_list>

static void put32(uint8_t *p, uint32_t v) {
  for (unsigned i = 0; i < 4; ++i) p[i] = uint8_t(v >> (i * 8));
}

int main() {
  uint8_t h[54] = {'B', 'M'};
  put32(h + 10, 54); put32(h + 14, 40);
  put32(h + 18, 448); put32(h + 22, 368);
  h[26] = 1; h[28] = 24;
  BmpLayout b{};
  const uint64_t fileSize = 54 + 448 * 3 * 368;
  assert(parseBmpHeader(h, sizeof(h), fileSize, b));
  assert(b.width == 448 && b.rows == 368 && b.rowBytes == 1344);
  assert(!parseBmpHeader(nullptr, 54, fileSize, b));
  for (size_t n = 0; n < 54; ++n) assert(!parseBmpHeader(h, n, fileSize, b));
  assert(!parseBmpHeader(h, sizeof(h), fileSize - 1, b));
  put32(h + 22, uint32_t(-368));
  assert(parseBmpHeader(h, sizeof(h), fileSize, b) && b.height == -368);
  for (int32_t bad : {0, 4097, -4097, std::numeric_limits<int32_t>::min()}) {
    put32(h + 22, uint32_t(bad)); assert(!parseBmpHeader(h, sizeof(h), fileSize, b));
  }
  put32(h + 22, 368);
  for (int32_t bad : {0, -1, 4097}) {
    put32(h + 18, uint32_t(bad)); assert(!parseBmpHeader(h, sizeof(h), fileSize, b));
  }
  put32(h + 18, 448);
  h[26] = 2; assert(!parseBmpHeader(h, sizeof(h), fileSize, b)); h[26] = 1;
  h[28] = 32; assert(!parseBmpHeader(h, sizeof(h), fileSize, b)); h[28] = 24;
  put32(h + 30, 1); assert(!parseBmpHeader(h, sizeof(h), fileSize, b)); put32(h + 30, 0);
  put32(h + 10, 53); assert(!parseBmpHeader(h, sizeof(h), fileSize, b));
  put32(h + 10, 54); put32(h + 14, 0xffffffffU);
  assert(!parseBmpHeader(h, sizeof(h), fileSize, b)); put32(h + 14, 40);
  // Row padding and little-endian high bits are tested independently.
  put32(h + 18, 1); put32(h + 22, 1);
  assert(parseBmpHeader(h, sizeof(h), 58, b) && b.rowBytes == 4);
  const uint8_t high[] = {0xff, 0xff, 0xff, 0xff}; assert(bmpRead32(high) == 0xffffffffU);
  assert(rgb565(255, 0, 0) == 0xf800);
  assert(rgb565(0, 255, 0) == 0x07e0);
  assert(rgb565(0, 0, 255) == 0x001f);
  for (const auto &p : PALETTES) {
    uint16_t lut[256]; buildPaletteLut(p, lut);
    assert(lut[0] == rgb565(p.stops[0].r, p.stops[0].g, p.stops[0].b));
    const auto &last = p.stops[p.count - 1];
    assert(lut[255] == rgb565(last.r, last.g, last.b));
  }
  return 0;
}
