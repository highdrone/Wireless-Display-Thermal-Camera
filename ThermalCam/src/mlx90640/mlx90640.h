#pragma once
// Melexis MLX90640 driver, plus the Wire bus its I2C functions use.
#include <Wire.h>

extern "C" {
#include "MLX90640_API.h"
#include "MLX90640_I2C_Driver.h"
}

void MLX90640_SetWire(TwoWire *wire);
