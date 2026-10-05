#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdarg.h>
#include <algorithm>
#include <string>
#include <strings.h>
#include <dirent.h>
typedef bool boolean;
typedef uint8_t byte;
#define PROGMEM
#define pgm_read_byte(a) (*(const uint8_t *)(a))
#define pgm_read_word(a) (*(const uint16_t *)(a))
#define pgm_read_dword(a) (*(const uint32_t *)(a))
#define pgm_read_pointer(a) (*(void *const *)(a))
#define LOW 0
#define HIGH 1
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
using std::max;
using std::min;
uint32_t millis();
void delay(uint32_t ms);
int digitalRead(int pin);
void pinMode(int pin, int mode);
void digitalWrite(int pin, int v);
#include "WString.h"
#include "Print.h"
class HostSerial : public Print {
 public:
  void begin(int) {}
  size_t write(uint8_t c) override { return fputc(c, stdout) == EOF ? 0 : 1; }
  void flush() { fflush(stdout); }
};
extern HostSerial Serial;
// FreeRTOS bits used by the sketch
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(x) (void)(x)
#define portEXIT_CRITICAL(x) (void)(x)
#define pdMS_TO_TICKS(x) (x)
inline void vTaskDelay(int) {}
typedef void (*TaskFunction_t)(void *);
inline int xTaskCreatePinnedToCore(TaskFunction_t, const char *, int, void *, int, void **, int) { return 1; }
struct HostESP { uint32_t getPsramSize() { return 8u << 20; } };
static HostESP ESP;
#define IRAM_ATTR
#define FALLING 2
inline void attachInterrupt(int, void (*)(void), int) {}
typedef void *TaskHandle_t;
inline void *ps_malloc(size_t n) { return malloc(n); }
