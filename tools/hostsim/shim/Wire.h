#pragma once
#include <Arduino.h>
class TwoWire {
 public:
  bool begin(int sda = -1, int scl = -1, uint32_t = 0) { pinSda = sda; pinScl = scl; return true; }
  void end() { pinSda = pinScl = -1; }
  void setClock(uint32_t) {}
  void beginTransmission(uint8_t) {}
  size_t write(uint8_t) { return 1; }
  // Nothing answers, unless a test puts a sensor on these pins.
  uint8_t endTransmission(bool = true) { return (simSensorSda >= 0 && pinSda == simSensorSda && pinScl == simSensorScl) ? 0 : 2; }
  int pinSda = -1, pinScl = -1;
  static inline int simSensorSda = -1, simSensorScl = -1;
  uint8_t requestFrom(uint8_t, uint8_t) { return 0; }
  int read() { return -1; }
};
extern TwoWire Wire, Wire1;
