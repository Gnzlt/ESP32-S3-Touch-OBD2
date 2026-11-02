#include <stdio.h>
#include <string.h>
#include "dtc.h"

void dtc_format(uint8_t a, uint8_t b, char out[DTC_LEN])
{
    static const char system[] = "PCBU";
    snprintf(out, DTC_LEN, "%c%d%X%02X", system[(a >> 6) & 3], (a >> 4) & 3, a & 0xF, b);
}

int dtc_decode_reply(const elm_result_t *r, int mode, bool is_can, char out[DTC_MAX][DTC_LEN])
{
    int count = 0;
    for (int m = 0; m < r->n_msgs; m++) {
        const uint8_t *msg = r->msg[m];
        int len = r->msg_len[m];
        if (len < 1 || msg[0] != 0x40 + mode) {
            continue;
        }
        int i = 1;
        int end = len;
        if (is_can) {
            if (len < 2) {
                continue;
            }
            int n = msg[1];
            i = 2;
            if (i + n * 2 < end) {
                end = i + n * 2;
            }
        }
        while (i + 1 < end && count < DTC_MAX) {
            if (msg[i] != 0 || msg[i + 1] != 0) {
                char code[DTC_LEN];
                dtc_format(msg[i], msg[i + 1], code);
                bool dup = false;
                for (int k = 0; k < count; k++) {
                    if (strcmp(out[k], code) == 0) dup = true;
                }
                if (!dup) {
                    strcpy(out[count++], code);
                }
            }
            i += 2;
        }
    }
    return count;
}

typedef struct {
    const char *code;
    const char *text;
} dtc_text_t;

// Generic SAE J2012 codes that the sensors on a fuel-injected single or twin
// can set. Maker-specific codes fall back to their group below.
static const dtc_text_t TEXTS[] = {
    {"P0030", "O2 sensor heater circuit (B1S1)"},
    {"P0031", "O2 sensor heater low (B1S1)"},
    {"P0032", "O2 sensor heater high (B1S1)"},
    {"P0105", "MAP sensor circuit"},
    {"P0106", "MAP sensor range/performance"},
    {"P0107", "MAP sensor low"},
    {"P0108", "MAP sensor high"},
    {"P0110", "Intake air temp circuit"},
    {"P0112", "Intake air temp sensor low"},
    {"P0113", "Intake air temp sensor high"},
    {"P0115", "Coolant temp circuit"},
    {"P0116", "Coolant temp range/performance"},
    {"P0117", "Coolant temp sensor low"},
    {"P0118", "Coolant temp sensor high"},
    {"P0120", "Throttle position circuit"},
    {"P0121", "Throttle position range/performance"},
    {"P0122", "Throttle position sensor low"},
    {"P0123", "Throttle position sensor high"},
    {"P0130", "O2 sensor circuit (B1S1)"},
    {"P0131", "O2 sensor low voltage (B1S1)"},
    {"P0132", "O2 sensor high voltage (B1S1)"},
    {"P0133", "O2 sensor slow response (B1S1)"},
    {"P0134", "O2 sensor no activity (B1S1)"},
    {"P0135", "O2 sensor heater circuit (B1S1)"},
    {"P0170", "Fuel trim (bank 1)"},
    {"P0171", "System too lean (bank 1)"},
    {"P0172", "System too rich (bank 1)"},
    {"P0201", "Injector circuit, cylinder 1"},
    {"P0202", "Injector circuit, cylinder 2"},
    {"P0230", "Fuel pump primary circuit"},
    {"P0300", "Random/multiple misfire"},
    {"P0301", "Misfire, cylinder 1"},
    {"P0302", "Misfire, cylinder 2"},
    {"P0335", "Crankshaft position sensor circuit"},
    {"P0336", "Crankshaft position sensor range"},
    {"P0340", "Camshaft position sensor circuit"},
    {"P0351", "Ignition coil A circuit"},
    {"P0352", "Ignition coil B circuit"},
    {"P0420", "Catalyst efficiency below threshold"},
    {"P0443", "EVAP purge valve circuit"},
    {"P0500", "Vehicle speed sensor"},
    {"P0505", "Idle air control system"},
    {"P0506", "Idle speed lower than expected"},
    {"P0507", "Idle speed higher than expected"},
    {"P0560", "System voltage"},
    {"P0562", "System voltage low"},
    {"P0563", "System voltage high"},
    {"P0600", "ECU serial communication link"},
    {"P0601", "ECU memory checksum error"},
    {"P0602", "ECU programming error"},
    {"P0605", "ECU ROM error"},
    {"P0606", "ECU processor fault"},
    {"P0627", "Fuel pump control circuit"},
    {"P0657", "Actuator supply voltage circuit"},
    {"P0705", "Gear position sensor circuit"},
    {"P0850", "Park/neutral switch circuit"},
    {"U0001", "High-speed CAN bus"},
    {"U0073", "CAN bus off"},
    {"U0100", "Lost communication with ECU"},
    {"U0121", "Lost communication with ABS"},
    {"U0155", "Lost communication with instruments"},
};

const char *dtc_describe(const char *code)
{
    for (size_t i = 0; i < sizeof(TEXTS) / sizeof(TEXTS[0]); i++) {
        if (strcmp(TEXTS[i].code, code) == 0) {
            return TEXTS[i].text;
        }
    }
    if (strlen(code) < 3) {
        return "Unknown code";
    }
    if (code[0] == 'P' && code[1] == '0') {
        switch (code[2]) {
        case '0':
        case '1':
        case '2': return "Fuel and air metering";
        case '3': return "Ignition or misfire";
        case '4': return "Emission controls";
        case '5': return "Speed, idle or inputs";
        case '6': return "ECU or outputs";
        case '7':
        case '8':
        case '9': return "Transmission";
        }
    }
    switch (code[0]) {
    case 'P': return "Manufacturer powertrain code";
    case 'C': return "Chassis (ABS) code";
    case 'B': return "Body code";
    case 'U': return "Network code";
    }
    return "Unknown code";
}
