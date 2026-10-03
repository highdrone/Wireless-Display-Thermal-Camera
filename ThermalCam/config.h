#pragma once
// ============================================================================
//  User settings. Change these, then re-upload.
// ============================================================================

// ---- Thermal sensor wiring -------------------------------------------------
// Default: the sensor shares the board's own I2C bus (the I2C solder pads,
// GPIO15 = SDA, GPIO14 = SCL). The touch chip, PMU, RTC and IMU are on the
// same bus; the MLX90640 lives at 0x33 so there is no address clash.
//
// If you instead wire SDA/SCL to two of the spare GPIO pads, put those GPIO
// numbers here. The sketch then gives the sensor its own I2C controller and
// runs it at 800 kHz, which allows the faster SENSOR_REFRESH below.
#define THERMAL_SDA 15
#define THERMAL_SCL 14

// Sensor sub-page rate: 4, 8, 16 or 32. Each sub-page refreshes half the
// pixels. 32 only keeps up on a dedicated bus, and higher rates add noise.
#define SENSOR_REFRESH_HZ 16

// How much the picture is smoothed over time, from 0 (off: raw and jumpy) to
// 0.9 (very smooth, but slow to follow movement). The readouts and the color
// scale settle more gently still.
#define SMOOTHING 0.6f

// ---- Picture orientation ---------------------------------------------------
// 1 or 3 = landscape (big picture). Pick whichever is right-side up for how
// you hold the board. 0 or 2 = portrait (smaller picture).
#define SCREEN_ROTATION 1

// Mirror the picture left/right. Turn this on if the sensor points away from
// you (like a normal camera) and the picture moves the wrong way when you pan.
#define MIRROR_IMAGE true
// Turn the picture upside down (if the sensor is mounted the other way up).
#define FLIP_IMAGE false

// ---- Display ---------------------------------------------------------------
#define SCREEN_BRIGHTNESS 200   // 0-255
// Radius of the screen's rounded corners, in pixels. Text near the corners is
// moved inward to clear them. Raise it if corner text is still clipped.
#define SCREEN_CORNER_RADIUS 48
#define START_IN_FAHRENHEIT false

// ---- Wireless screen -------------------------------------------------------
// Flash this same firmware to a second ESP32-S3-Touch-AMOLED-1.8 with no
// thermal sensor: it becomes a wireless screen showing the camera's picture
// (ESP-NOW radio, no router needed). The camera only transmits while a screen
// is listening, and doesn't auto-off while one is. The radio does use extra
// battery; set false to turn it off. A board without a sensor then keeps
// looking for one instead.
#define WIRELESS_SCREEN true
// Radio channel, 1-13. Both boards must use the same one.
#define WIRELESS_CHANNEL 1

// ---- Auto-off --------------------------------------------------------------
// Turn off after this many seconds without a button press or screen tap
// (0 = never). On battery the board powers off; on USB power only the screen
// turns off.
#define IDLE_OFF_SECONDS 60
// Show a "tap screen to keep using" countdown for this many seconds first.
#define IDLE_WARNING_SECONDS 10

// The color scale always stretches over at least this many degrees C. This
// stops sensor noise from looking like a rainbow when everything in view is
// the same temperature.
#define MIN_SPAN_C 3.0f
