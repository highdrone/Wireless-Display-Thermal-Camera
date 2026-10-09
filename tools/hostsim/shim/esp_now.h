#pragma once
// Host stand-in: sent packets are kept so a test can replay them.
#include <Arduino.h>
#include <vector>
#include <cstddef>
#define ESP_OK 0
#define ESP_ERR_ESPNOW_NO_MEM 0x3067
typedef int esp_err_t;
typedef struct { uint8_t *src_addr; uint8_t *des_addr; void *rx_ctrl; } esp_now_recv_info_t;
typedef struct { uint8_t peer_addr[6]; uint8_t lmk[16]; uint8_t channel; int ifidx; bool encrypt; void *priv; } esp_now_peer_info_t;
typedef void (*esp_now_recv_cb_t)(const esp_now_recv_info_t *, const uint8_t *, int);
extern std::vector<std::vector<uint8_t>> sentPackets;
extern void (*onSendHook)(const uint8_t *, size_t);
inline int esp_now_init() { return ESP_OK; }
inline int esp_now_register_recv_cb(esp_now_recv_cb_t) { return ESP_OK; }
inline int esp_now_add_peer(const esp_now_peer_info_t *) { return ESP_OK; }
typedef struct { int phymode; int rate; bool ersu; bool dcm; } esp_now_rate_config_t;
inline int esp_now_set_peer_rate_config(const uint8_t *, esp_now_rate_config_t *) { return ESP_OK; }
inline uint32_t simEspNowVersion = 2;
inline int esp_now_get_version(uint32_t *v) { *v = simEspNowVersion; return ESP_OK; }
inline int esp_now_send(const uint8_t *, const uint8_t *data, size_t len) {
  sentPackets.emplace_back(data, data + len);
  if (onSendHook) onSendHook(data, len);
  return ESP_OK;
}
