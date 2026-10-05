#pragma once
#include "Arduino_GFX.h"
#include "canvas/Arduino_Canvas.h"
class Arduino_ESP32QSPI : public Arduino_DataBus {
 public:
  Arduino_ESP32QSPI(int8_t, int8_t, int8_t, int8_t, int8_t, int8_t, bool = false) {}
  bool begin(int32_t, int8_t) override { return true; }
  void beginWrite() override {}
  void endWrite() override {}
  void writeCommand(uint8_t) override {}
  void writeCommand16(uint16_t) override {}
  void writeCommandBytes(uint8_t *, uint32_t) override {}
  void write(uint8_t) override {}
  void write16(uint16_t) override {}
  void writeRepeat(uint16_t, uint32_t) override {}
  void writeBytes(uint8_t *, uint32_t) override {}
  void writePixels(uint16_t *, uint32_t) override {}
};
// The "panel" is a canvas too, so flush() lands in a buffer we can save as an image.
class Arduino_OLED : public Arduino_Canvas {
 public:
  Arduino_OLED(int16_t w, int16_t h) : Arduino_Canvas(w, h, nullptr) {}
  virtual void setBrightness(uint8_t) {}
};
class Arduino_SH8601 : public Arduino_OLED {
 public:
  Arduino_SH8601(Arduino_DataBus *, int8_t, uint8_t, int16_t w, int16_t h, uint8_t = 0, uint8_t = 0, uint8_t = 0, uint8_t = 0) : Arduino_OLED(w, h) {}
};
class Arduino_CO5300 : public Arduino_OLED {
 public:
  Arduino_CO5300(Arduino_DataBus *, int8_t, uint8_t, int16_t w, int16_t h, uint8_t = 0, uint8_t = 0, uint8_t = 0, uint8_t = 0) : Arduino_OLED(w, h) {}
};
