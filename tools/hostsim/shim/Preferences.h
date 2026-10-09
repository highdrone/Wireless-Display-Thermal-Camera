#pragma once
#include <Arduino.h>
#include <map>
class Preferences {
 public:
  bool begin(const char *, bool) { return true; }
  uint8_t getUChar(const char *, uint8_t d) { return d; }
  bool getBool(const char *, bool d) { return d; }
  size_t putUChar(const char *, uint8_t) { return 1; }
  size_t putBool(const char *, bool) { return 1; }
  uint16_t getUShort(const char *, uint16_t d) { return shot ? shot : d; }
  size_t putUShort(const char *, uint16_t v) { shot = v; return 2; }
  uint16_t shot = 0;
  float getFloat(const char *k, float d) { auto i = floats.find(k); return i == floats.end() ? d : i->second; }
  size_t putFloat(const char *k, float v) { floats[k] = v; return 4; }
  std::map<std::string, float> floats;
};
