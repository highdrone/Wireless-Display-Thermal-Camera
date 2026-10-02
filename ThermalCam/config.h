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

// Sensor sub-page rate. One full image needs two sub-pages, so 16 Hz gives
// ~8 images per second. 32 Hz only keeps up on a dedicated bus, and
// higher rates add noise. Options: MLX90640_4_HZ, _8_HZ, _16_HZ, _32_HZ.
#define SENSOR_REFRESH MLX90640_16_HZ

// ---- Picture orientation ---------------------------------------------------
// Starting orientation. 1 or 3 = landscape, 0 or 2 = portrait (smaller
// picture, only without AUTO_ROTATE).
#define SCREEN_ROTATION 1

// Flip the screen and picture between the two landscape orientations to match
// how the board is held, using the board's built-in motion sensor.
#define AUTO_ROTATE true
// Set true if auto-rotate turns the screen upside down instead of right side up.
#define AUTO_ROTATE_INVERT false
// Set true once the thermal sensor is mounted on the board so it turns with
// it. The picture then stays put on the screen and only the text flips.
#define SENSOR_FIXED_TO_BOARD false

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
