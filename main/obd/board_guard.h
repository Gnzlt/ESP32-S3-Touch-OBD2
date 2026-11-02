/*
 * Looking after the board itself: its LiPo and its temperature.
 *
 * - Hot (a warning level, alerts.h): battery saver goes on, to draw and heat
 *   less. It goes off again once both parts are TEMP_REARM_C below their
 *   warning levels, but only if the heat turned it on, the user hasn't
 *   touched the switch since, and the battery isn't low.
 * - Too hot (a critical level for GUARD_HOT_HOLD_MS) or the battery empty
 *   (under GUARD_EMPTY_MV for GUARD_EMPTY_HOLD_MS, not charging): the board
 *   switches itself off. Software can't open the PWR latch, so "off" is the
 *   ride closed, the card unmounted and deep sleep, waking every
 *   GUARD_SLEEP_S to check whether it may start again (guard_may_wake).
 *
 * Without it, a long ride ran the cell down to 2.9 V with the engine still
 * running, and the ride's end was never written. Pure C, host-testable.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define GUARD_EMPTY_MV      3400    // under load; the gauge's own 0 % (EDV0) is 3490 mV
#define GUARD_EMPTY_HOLD_MS 30000
#define GUARD_WAKE_MV       3700    // a rested cell this full may start again
#define GUARD_HOT_HOLD_MS   10000
#define GUARD_SLEEP_S       300
#define BOARD_BATT_LOW_PCT  20      // the first low-battery alert (alerts.c)

typedef enum {
    GUARD_NONE,
    GUARD_SAVER_ON,           // too warm: turn battery saver on
    GUARD_SAVER_OFF,          // cooled down: turn off the saver the heat turned on
    GUARD_OFF_EMPTY,          // switch off: the battery is empty
    GUARD_OFF_HOT,            // switch off: too hot
} guard_action_t;

typedef struct {
    int64_t low_since, crit_since;   // -1 = not now
    bool heat_saver;                 // the heat turned battery saver on
    bool heat_armed;                 // cool since the heat last turned it on: it may again
} board_guard_t;

typedef struct {
    bool has_chip, has_cell;         // the temperatures are known
    float chip_c, cell_c;
    bool battery;                    // a cell is fitted and was read
    int mv;
    bool charging;
    bool low;                        // the charge is at or under the low-battery alert (20 %)
    bool saver;                      // battery saver is on now
} guard_in_t;

void guard_init(board_guard_t *g);
// With each battery and temperature reading. The caller does what it returns.
guard_action_t guard_update(board_guard_t *g, const guard_in_t *in, int64_t now);
// Battery saver was switched by someone else (the Status page, the battery
// alert): the heat no longer owns it.
void guard_saver_changed(board_guard_t *g);
// At a wake from the guard's deep sleep: may the board start again?
bool guard_may_wake(guard_action_t why, const guard_in_t *in);
