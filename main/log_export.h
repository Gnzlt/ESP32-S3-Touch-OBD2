/*
 * Ride log download over the USB cable. The host sends text commands on the
 * USB-Serial-JTAG port and gets the SD card's ride files back, base64-framed:
 *
 *   obd2 ls                          -> "@LS ride_0001/trip.csv 12345" ... "@LS fillups.csv 321" "@END"
 *   obd2 get ride_0001/trip.csv [offset]
 *                                    -> "@FILE <path> <size>", base64 lines, "@EOF <crc32 hex>"
 *   obd2 sync <epoch> <next_ride>    -> "@OK <clock was set 0|1> <ride_next>": sets the board's clock (UTC)
 *                                       and keeps ride numbers at or above next_ride
 *
 * Errors come back as "@ERR <reason>". tools/pull_logs.py is the host side.
 */
#pragma once

void log_export_start(void);
