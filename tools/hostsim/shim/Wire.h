#pragma once
#include <Arduino.h>
class TwoWire {
 public:
  bool begin(int = -1, int = -1, uint32_t = 0) { return true; }
  void beginTransmission(uint8_t) {}
  size_t write(uint8_t) { return 1; }
  uint8_t endTransmission(bool = true) { return 2; }  // nothing answers
  uint8_t requestFrom(uint8_t, uint8_t) { return 0; }
  int read() { return -1; }
};
extern TwoWire Wire, Wire1;
