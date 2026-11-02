/*
 * The board switching itself off for the guard (obd/board_guard.h): the PWR
 * latch is hardware only, so "off" is deep sleep with the backlight held off,
 * waking every GUARD_SLEEP_S to check whether it may start again. The rest of
 * the board (LCD controller, sensors, the buck) stays powered, so the cell
 * still drains slowly; not measured yet.
 */
#pragma once

#include "obd/board_guard.h"

// Deep sleep now. Doesn't return.
void board_power_sleep(guard_action_t why);

// First thing in app_main. After a guard sleep it reads the gauge and the
// chip's temperature: still empty or hot, it sleeps again (doesn't return).
// Returns why the board slept, or GUARD_NONE after a normal power-on or reset.
guard_action_t board_power_wake_check(void);

const char *board_power_reason(guard_action_t why);
