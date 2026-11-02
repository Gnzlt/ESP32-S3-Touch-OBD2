/*
 * Everything the UI, the alerts and the trip read about the bike: the latest
 * value of each PID with its timestamp, what the ECU supports, fault codes,
 * and the phase of the link. Written only by the ELM327 session (one task);
 * the UI reads a copy.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "pids.h"

typedef enum {
    PHASE_NO_BLE,     // Bluetooth failed to start
    PHASE_SCAN,       // looking for the adapter
    PHASE_CONNECT,    // found it, connecting and subscribing
    PHASE_INIT,       // talking to the ELM327 / searching for the ECU
    PHASE_ECU_OFF,    // the adapter answers but no ECU does (ignition off)
    PHASE_LIVE,       // polling
} obd_phase_t;

#define DTC_MAX 8
#define DTC_LEN 6     // "P0123" + NUL

typedef enum {
    CLEAR_NONE,
    CLEAR_PENDING,    // asked for, waiting for its turn
    CLEAR_SENT,
    CLEAR_OK,
    CLEAR_FAILED,
} clear_state_t;

typedef struct {
    obd_phase_t phase;

    // Support bitmap from 0100/0120/0140, indexed by PID.
    bool supported[PID_MAX + 1];
    bool support_known;

    bool mil;
    int dtc_count;                      // from PID 01, when the ECU has it
    int n_dtcs, n_pending;
    char dtcs[DTC_MAX][DTC_LEN];        // Mode 03, stored
    char pending[DTC_MAX][DTC_LEN];     // Mode 07, pending
    bool dtcs_read;                     // at least one Mode 03 answer this link
    clear_state_t clear;

    // For the status page.
    char protocol[4];
    char adapter[32];                   // the ATI answer, e.g. "ELM327 v2.3"
    char ble_name[32];                  // the advertised name, e.g. "IOS-Vlink"
    char ecu[12];                       // engine ECU reply id once locked, e.g. "18DAF110"
    char last_error[24];
    float replies_per_sec;
    bool multi_pid;
    uint32_t timeouts, reinits, link_drops, ecu_drops;
    int64_t last_reply_at;

    // Mode 09, read once per live session. Empty strings when the ECU
    // doesn't say.
    char vin[18];
    char cal_id[2][17];                 // calibration IDs: the ECU software
    char cvn[2][9];                     // their verification numbers, as hex
    bool info_read;                     // the Mode 09 reads are done (answered or not)

    // Per PID: decoded value, raw bytes (bit fields like 41), when it was read.
    float val[PID_MAX + 1];
    uint8_t raw[PID_MAX + 1][4];
    int64_t at[PID_MAX + 1];
} obd_state_t;

void obd_state_init(obd_state_t *s);
void obd_state_set(obd_state_t *s, uint8_t pid, float value, int64_t now);

// The value if it is current (see STALE_MS in obd_state.c), else false.
bool obd_value(const obd_state_t *s, uint8_t pid, int64_t now, float *out);
// The value if it was read within max_age_ms.
bool obd_fresh(const obd_state_t *s, uint8_t pid, int64_t now, int max_age_ms, float *out);

void obd_forget_values(obd_state_t *s);
void obd_clear_support(obd_state_t *s);
bool obd_is_supported(const obd_state_t *s, uint8_t pid);

// Walk one Mode 01 reply (41 pid data [pid data ...]) and store every PID in
// it. Returns how many PIDs were decoded.
int obd_apply_mode01(obd_state_t *s, const uint8_t *msg, int len, int64_t now);

// One Mode 09 reply (49 pid ...): 02 VIN, 04 calibration IDs, 06 CVNs.
// Returns false for anything else.
bool obd_apply_mode09(obd_state_t *s, const uint8_t *msg, int len);

// The support bitmap for PIDs base+1..base+32 as 8 hex digits (+ NUL).
void obd_support_hex(const obd_state_t *s, uint8_t base, char out[9]);

void obd_set_error(obd_state_t *s, const char *text);
