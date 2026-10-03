#pragma once
// Copyright (c) 2026 ESP32 Thermal Camera contributors. MIT License.
// Validate untrusted SD-card headers before allocation, seeking or decoding.
#include <stddef.h>
#include <stdint.h>

struct BmpLayout {
  int32_t width, height, rows;
  uint32_t offset, rowBytes;
};

static inline uint32_t bmpRead32(const uint8_t *p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static inline bool parseBmpHeader(const uint8_t *hdr, size_t bytes, uint64_t fileBytes, BmpLayout &out) {
  if (!hdr || bytes < 54 || hdr[0] != 'B' || hdr[1] != 'M') return false;
  const uint32_t dib = bmpRead32(hdr + 14), offset = bmpRead32(hdr + 10);
  const int32_t w = int32_t(bmpRead32(hdr + 18)), h = int32_t(bmpRead32(hdr + 22));
  // Bound signed dimensions BEFORE negating: INT32_MIN cannot be abs()'d.
  if (w <= 0 || w > 4096 || h == 0 || h < -4096 || h > 4096) return false;
  if (dib < 40 || uint64_t(offset) < uint64_t(dib) + 14 || offset > fileBytes) return false;
  if (hdr[26] != 1 || hdr[27] != 0 || hdr[28] != 24 || hdr[29] != 0 || bmpRead32(hdr + 30) != 0) return false;
  const int32_t rows = h < 0 ? -h : h;
  const uint32_t rowBytes = (uint32_t(w) * 3 + 3) & ~uint32_t(3);
  if (uint64_t(offset) + uint64_t(rowBytes) * uint32_t(rows) > fileBytes) return false;
  out = {w, h, rows, offset, rowBytes};
  return true;
}
