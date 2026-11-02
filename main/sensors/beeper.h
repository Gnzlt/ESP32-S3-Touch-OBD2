#pragma once

#include "esp_err.h"

typedef enum {
    BEEP_URGENT,   // overheating: three high beeps
    BEEP_WARN,     // voltage: two
    BEEP_INFO,     // a new fault code: a rising pair
    BEEP_TICK,     // touch feedback
} beep_t;

esp_err_t beeper_init(void);
void beeper_play(beep_t b);
