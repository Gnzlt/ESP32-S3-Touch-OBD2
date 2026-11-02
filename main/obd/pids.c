#include <stddef.h>
#include "pids.h"

int pid_data_length(uint8_t pid)
{
    switch (pid) {
    case PID_SUPPORTED_00:
    case PID_SUPPORTED_20:
    case PID_SUPPORTED_40:
    case PID_STATUS:
    case PID_MONITOR:
        return 4;
    case PID_FREEZE_DTC:
    case PID_FUEL_STATUS:
    case PID_RPM:
    case PID_MAF:
    case PID_RUNTIME:
    case PID_MIL_DIST:
    case PID_VOLTAGE:
    case PID_FUEL_RATE:
        return 2;
    case PID_LOAD:
    case PID_COOLANT:
    case PID_STFT1:
    case PID_LTFT1:
    case PID_MAP:
    case PID_SPEED:
    case PID_ADVANCE:
    case PID_IAT:
    case PID_THROTTLE:
    case PID_SEC_AIR:
    case PID_OBD_STD:
    case PID_FUEL_LEVEL:
    case PID_AMBIENT:
    case PID_OIL_TEMP:
        return 1;
    }
    return -1;
}

float pid_decode(uint8_t pid, const uint8_t *d)
{
    int a = d[0];
    switch (pid) {
    case PID_RPM:
        return ((a * 256) + d[1]) / 4.0f;
    case PID_VOLTAGE:
        return ((a * 256) + d[1]) / 1000.0f;
    case PID_FUEL_RATE:
        return ((a * 256) + d[1]) / 20.0f;
    case PID_MAF:
        return ((a * 256) + d[1]) / 100.0f;
    case PID_RUNTIME:
    case PID_MIL_DIST:
        return (float)((a * 256) + d[1]);
    case PID_COOLANT:
    case PID_IAT:
    case PID_AMBIENT:
    case PID_OIL_TEMP:
        return (float)(a - 40);
    case PID_LOAD:
    case PID_THROTTLE:
    case PID_FUEL_LEVEL:
        return a * 100.0f / 255.0f;
    case PID_STFT1:
    case PID_LTFT1:
        return (a - 128) * 100.0f / 128.0f;
    case PID_ADVANCE:
        return a / 2.0f - 64.0f;
    case PID_MAP:
    case PID_SPEED:
    case PID_SEC_AIR:
    case PID_OBD_STD:
    case PID_FUEL_STATUS:
    case PID_FREEZE_DTC:
        return (float)a;
    }
    return 0.0f;
}

bool pid_is_bitmap(uint8_t pid)
{
    return pid == PID_SUPPORTED_00 || pid == PID_SUPPORTED_20 || pid == PID_SUPPORTED_40;
}

const char *pid_fuel_status_text(uint8_t a)
{
    switch (a) {
    case 0x00: return "Off";
    case 0x01: return "Open loop (cold)";
    case 0x02: return "Closed loop";
    case 0x04: return "Open loop (load)";
    case 0x08: return "Open loop (fault)";
    case 0x10: return "Closed loop (fault)";
    }
    return "Unknown";
}

const char *pid_obd_std_text(uint8_t a)
{
    switch (a) {
    case 1: return "OBD-II (CARB)";
    case 2: return "OBD (EPA)";
    case 3: return "OBD + OBD-II";
    case 4: return "OBD-I";
    case 5: return "Not OBD";
    case 6: return "EOBD";
    case 7: return "EOBD + OBD-II";
    case 8: return "EOBD + OBD";
    case 9: return "EOBD + OBD + OBD-II";
    case 10: return "JOBD";
    case 11: return "JOBD + OBD-II";
    case 12: return "JOBD + EOBD";
    case 13: return "JOBD + EOBD + OBD-II";
    case 14: return "OBD + EOBD + KOBD";
    case 15: return "OBD + OBD-II + EOBD + KOBD";
    case 17: return "EMD";
    case 18: return "EMD+";
    case 19: return "HD OBD-C";
    case 20: return "HD OBD";
    case 21: return "WWH OBD";
    case 23: return "HD EOBD-I";
    case 24: return "HD EOBD-I N";
    case 25: return "HD EOBD-II";
    case 26: return "HD EOBD-II N";
    case 27: return "HD ZEV";
    case 28: return "OBDBr-1";
    case 29: return "OBDBr-2";
    case 30: return "KOBD";
    case 31: return "IOBD I";
    case 32: return "IOBD II";
    case 33: return "HD EOBD-IV";
    case 34: return "OBD + OBD-II + HD OBD";
    case 35: return "OBDBr-3";
    }
    // Not in the public J1979 list (some ECUs answer 36): the caller shows
    // "code N" rather than a guess.
    return NULL;
}
