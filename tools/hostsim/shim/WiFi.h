#pragma once
#include <Arduino.h>
#define WIFI_STA 1
class WiFiClass { public: bool mode(int) { return true; } };
static WiFiClass WiFi;
