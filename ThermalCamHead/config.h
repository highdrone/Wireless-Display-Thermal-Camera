#pragma once
// ============================================================================
//  Settings for the thermal + color camera head (Heltec HT-HC33).
//  Change these, then re-upload.
// ============================================================================

// ---- Thermal sensor (MLX90640) wiring --------------------------------------
// Solder the sensor's SDA and SCL to these GPIOs on the HT-HC33's J1 header.
// They are the board's microSD-slot lines, so leave that slot empty. If the
// sensor doesn't answer there, the head also tries SDA and SCL swapped, and
// the other pair of SD-slot pins.
#define THERMAL_SDA 11      // J1 "SD_MOSI"
#define THERMAL_SCL 10      // J1 "SD_CS"
#define THERMAL_ALT_SDA 16  // J1 "SD_MISO"
#define THERMAL_ALT_SCL 15  // J1 "SD_CLK"

// Sensor sub-page rate: 4, 8, 16 or 32. The sensor has its own bus here, so
// 32 keeps up, but it is noisier.
#define SENSOR_REFRESH_HZ 16
// Smoothing over time: 0 = off .. 0.9 = very steady but slow to follow.
#define SMOOTHING 0.6f
// The color scale always stretches over at least this many degrees C.
#define MIN_SPAN_C 3.0f

// ---- Color camera (OV3660) -------------------------------------------------
// FRAMESIZE_QVGA (320x240) streams about 10 pictures a second. FRAMESIZE_VGA
// (640x480) is sharper, which helps with the narrow 55-degree thermal
// sensor, but streams fewer pictures and needs a strong radio link.
#define COLOR_FRAME_SIZE FRAMESIZE_QVGA
#define COLOR_JPEG_QUALITY 14  // 10 (best) .. 40 (smallest)
#define COLOR_MAX_FPS 10
// Turn the color picture the right way round. Heltec's own example flips the
// HT-HC33 camera vertically.
#define COLOR_VFLIP true
#define COLOR_HMIRROR false

// ---- Radio -----------------------------------------------------------------
// Must match the wireless screen's WIRELESS_CHANNEL.
#define WIRELESS_CHANNEL 1
// Transmit rate for the picture stream. 11 Mbps (802.11b) carries the color
// pictures with good range; WIFI_PHY_RATE_24M is faster but reaches less far.
#define RADIO_RATE WIFI_PHY_RATE_11M_L

// ---- Standby ---------------------------------------------------------------
// With no wireless screen listening for this many seconds (from startup or
// after the screen goes away), the head goes into standby: it sleeps, wakes
// every STANDBY_CHECK_SECONDS to listen for a switched-on screen, and starts
// streaming again when it hears one. After STANDBY_MINUTES it switches off;
// then the USER key turns it on. 0 seconds = never go into standby.
#define CAMERA_LINK_TIMEOUT_SECONDS 20
#define STANDBY_CHECK_SECONDS 5
#define STANDBY_MINUTES 30

// ---- Serial output privacy -------------------------------------------------
// Leave disabled when sharing logs: prints the sensor's unique ID.
#define LOG_SENSOR_SERIAL false
