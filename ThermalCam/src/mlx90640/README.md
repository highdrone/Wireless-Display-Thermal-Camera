Melexis MLX90640 driver, from https://github.com/melexis/mlx90640-library
(commit f6be7ca, Apache License 2.0, see LICENSE).

Changes: the `#include <...>` lines in MLX90640_API.c use quotes so the files
build from this folder. MLX90640_I2C_Driver.cpp and mlx90640.h are new: they
implement the driver's I2C functions with the Arduino Wire library.
