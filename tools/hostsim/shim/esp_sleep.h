#pragma once
// Host stand-in: deep sleep throws so the test can see it happen.
#include <stdint.h>
#include <stdexcept>
typedef enum { ESP_SLEEP_WAKEUP_UNDEFINED, ESP_SLEEP_WAKEUP_EXT0 = 2, ESP_SLEEP_WAKEUP_EXT1 = 3, ESP_SLEEP_WAKEUP_TIMER = 4 } esp_sleep_wakeup_cause_t;
enum { ESP_EXT1_WAKEUP_ANY_LOW = 0 };
enum { ESP_PD_DOMAIN_RTC_PERIPH = 0 };
enum { ESP_PD_OPTION_ON = 1 };
#define RTC_DATA_ATTR
struct DeepSleep : std::runtime_error { DeepSleep() : std::runtime_error("deep sleep") {} };
extern esp_sleep_wakeup_cause_t simWakeCause;
extern uint64_t simTimerUs, simExt1Mask;
inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause() { return simWakeCause; }
inline int esp_sleep_enable_timer_wakeup(uint64_t us) { simTimerUs = us; return 0; }
inline int esp_sleep_enable_ext1_wakeup(uint64_t mask, int) { simExt1Mask = mask; return 0; }
inline int esp_sleep_pd_config(int, int) { return 0; }
inline int simExt0Pin = -1;
inline int esp_sleep_enable_ext0_wakeup(int pin, int) { simExt0Pin = pin; return 0; }
[[noreturn]] inline void esp_deep_sleep_start() { throw DeepSleep(); }
