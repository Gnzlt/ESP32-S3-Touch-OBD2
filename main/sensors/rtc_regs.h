/*
 * The PCF85063A's time registers (0x04-0x0A: seconds, minutes, hours, day,
 * weekday, month, year), BCD, to and from Unix time in UTC. The year register
 * counts from 2000. Bit 7 of the seconds register (OS) is set when the
 * oscillator stopped, a power loss: the time can't be trusted until it is set
 * again. Pure logic.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define RTC_REGS_FIRST 0x04
#define RTC_REGS_N     7

// Seconds since 1970 (2000-01-01 to 2099-12-31) to the 7 registers, OS clear.
void rtc_regs_encode(int64_t epoch, uint8_t regs[RTC_REGS_N]);

// False when OS is set or a register isn't a valid date or time.
bool rtc_regs_decode(const uint8_t regs[RTC_REGS_N], int64_t *epoch);
