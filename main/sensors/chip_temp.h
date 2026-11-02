/*
 * The ESP32-S3's own temperature sensor: the die, which runs about 15 °C above
 * the air around the board. Range 20-100 °C (the driver's most accurate band
 * for a warm board).
 */
#pragma once

#include <stdbool.h>

bool chip_temp_init(void);
// False when the sensor didn't start or the read failed.
bool chip_temp_read(float *celsius);
