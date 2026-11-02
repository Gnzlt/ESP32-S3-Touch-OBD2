/*
 * The board's PCF85063A clock (I2C 0x51), which runs from the LiPo while the
 * board is off. It holds UTC. tools/pull_logs.py sets it from the Mac at each
 * download (log_export.c's "sync"); at boot a set clock sets the system time,
 * so rides on the card get a date.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Read the clock and, if it holds a time, set the system time from it.
void wallclock_init(void);

// Set the clock and the system time (seconds since 1970, UTC).
bool wallclock_set(int64_t epoch);

// The system time is a real date (set from the clock or by a sync).
bool wallclock_valid(void);
