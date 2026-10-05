#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "WString.h"
class Print {
 public:
  virtual ~Print() {}
  virtual size_t write(uint8_t) = 0;
  virtual size_t write(const uint8_t *b, size_t n) { size_t r = 0; while (n--) r += write(*b++); return r; }
  size_t write(const char *s) { return write((const uint8_t *)s, strlen(s)); }
  size_t print(const char *s) { return write(s); }
  size_t print(const __FlashStringHelper *s) { return write((const char *)s); }
  size_t print(char c) { return write((uint8_t)c); }
  size_t print(const String &s) { return write(s.c_str()); }
  size_t print(int v) { char b[16]; snprintf(b, 16, "%d", v); return write(b); }
  size_t println(const char *s = "") { return print(s) + write("\n"); }
  size_t printf(const char *f, ...) { char b[512]; va_list a; va_start(a, f); vsnprintf(b, sizeof b, f, a); va_end(a); return write(b); }
};
