#pragma once
#define WIFI_SECOND_CHAN_NONE 0
#define WIFI_IF_STA 0
inline int esp_wifi_set_channel(int, int) { return 0; }
enum { WIFI_PS_NONE = 0 };
inline int esp_wifi_set_ps(int) { return 0; }
enum { WIFI_PHY_MODE_LR, WIFI_PHY_MODE_11B, WIFI_PHY_MODE_11G, WIFI_PHY_MODE_HT20 };
enum { WIFI_PHY_RATE_11M_L = 0x03, WIFI_PHY_RATE_11M_S = 0x07, WIFI_PHY_RATE_24M = 0x09, WIFI_PHY_RATE_12M = 0x0A, WIFI_PHY_RATE_6M = 0x0B };
