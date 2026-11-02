/*
 * Standard OBD2 Mode 01 PIDs this app understands: the data length of each and
 * the SAE J1979 formula that turns its bytes into a value. A PID missing here
 * is never requested, and a reply that names one ends the walk through a
 * message. Besides the usual ones it decodes PIDs bikes often report: 03, 06,
 * 12, 1C, 21, 41.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define PID_SUPPORTED_00 0x00   // bitmap of PIDs 01-20
#define PID_STATUS       0x01   // MIL + DTC count (+ readiness)
#define PID_FREEZE_DTC   0x02
#define PID_FUEL_STATUS  0x03   // fuel system status (open/closed loop)
#define PID_LOAD         0x04
#define PID_COOLANT      0x05
#define PID_STFT1        0x06   // short-term fuel trim, bank 1
#define PID_LTFT1        0x07
#define PID_MAP          0x0B
#define PID_RPM          0x0C
#define PID_SPEED        0x0D
#define PID_ADVANCE      0x0E
#define PID_IAT          0x0F
#define PID_MAF          0x10
#define PID_THROTTLE     0x11
#define PID_SEC_AIR      0x12   // commanded secondary air status
#define PID_OBD_STD      0x1C   // which OBD standard the ECU conforms to
#define PID_RUNTIME      0x1F
#define PID_SUPPORTED_20 0x20   // bitmap of PIDs 21-40
#define PID_MIL_DIST     0x21   // km driven with the MIL on
#define PID_FUEL_LEVEL   0x2F
#define PID_SUPPORTED_40 0x40   // bitmap of PIDs 41-60
#define PID_MONITOR      0x41   // monitor status this drive cycle
#define PID_VOLTAGE      0x42
#define PID_AMBIENT      0x46
#define PID_OIL_TEMP     0x5C
#define PID_FUEL_RATE    0x5E

// The highest PID any table here indexes; obd_state sizes its arrays by it.
#define PID_MAX 0x60

// Number of data bytes after the PID byte, or -1 if unknown.
int pid_data_length(uint8_t pid);

// Decode the data bytes of a PID that starts at data[0]. The caller has
// checked pid_data_length(pid) bytes are present.
float pid_decode(uint8_t pid, const uint8_t *data);

// Bitmap PIDs (00, 20, 40): bit 31 of the 4 bytes is PID base+1.
bool pid_is_bitmap(uint8_t pid);

// Text for PID 03 (fuel system 1 status byte), e.g. "Closed loop".
const char *pid_fuel_status_text(uint8_t a);

// Text for PID 1C, e.g. "EOBD", "OBD-II"; NULL for types not listed here.
const char *pid_obd_std_text(uint8_t a);
