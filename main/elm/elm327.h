/*
 * The ELM327 session on top of a transport (the BLE link): initialise the
 * adapter, find the ECU and its protocol, read what it supports, then poll
 * forever. One command is in flight at a time; the next goes out from the
 * previous reply. Timeouts and retries run from elm_tick(), every 20 ms.
 * Also Mode 04 (clear codes) and Mode 09 (VIN, calibration IDs). Pure C: no FreeRTOS, so it runs in host tests.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "elm_reply.h"
#include "obd/obd_state.h"
#include "obd/poller.h"

typedef struct {
    // Queue bytes for the adapter. False if the link is not ready.
    bool (*send)(void *ctx, const uint8_t *data, int len);
    // Drop the link and look for the adapter again.
    void (*restart)(void *ctx);
    // A reply timed out: forget the half-received one.
    void (*discard_partial)(void *ctx);
    // The protocol digit the search found, to try first next time.
    void (*save_protocol)(void *ctx, const char *digit);
    void *ctx;
} elm_transport_t;

typedef enum { S_IDLE, S_INIT, S_DETECT, S_ECU_OFF, S_LIVE } elm_session_t;

typedef enum {
    K_NONE,
    K_INIT,        // AT setup script
    K_DETECT,      // 0100: is an ECU there, and on which protocol
    K_DPN,         // ATDPN: which protocol the search found
    K_BITMAP,      // 0120 / 0140
    K_LOCK,        // ATSH / ATCRA: address the engine ECU only
    K_LOCK_CHECK,  // 0100 under the lock
    K_UNLOCK,      // ATAR / ATSH back to the defaults
    K_POLL,
    K_DTC,
    K_CLEAR,       // 04
    K_INFO,        // Mode 09: 0900 bitmap, then VIN / calibration IDs / CVNs
} elm_kind_t;

#define ELM_SCRIPT_MAX 16

typedef struct {
    obd_state_t *state;
    elm_transport_t io;
    poller_t poller;

    elm_session_t s;
    elm_kind_t kind;
    bool in_flight;
    char cmd[24];               // the command in flight, with its CR
    int64_t sent_at;
    int timeout;
    int step;
    int n_script;
    char script[ELM_SCRIPT_MAX][24];
    bool weak;                  // a clone that refused ATS0 or ATAT1
    char protocol_setting[2];   // "" = automatic
    int n_lock, lock_step;
    char lock[2][20];
    poll_job_t *job;
    int dtc_mode;
    int info_step;              // Mode 09 progress this live session; > N_INFO = done
    uint8_t info_pid, info_bitmap;
    uint8_t bitmap_queue[2];
    int n_bitmap_queue;
    int64_t retry_at;
    int failures, timeouts, detect_timeouts, ecu_off_misses;
    int64_t last_decode_at;     // the last ECU reply that carried a value
    bool reinit_pending;
    bool unsynced;              // since a timeout: a late reply may still come
    char protocol[2];          // ATSP argument: "6", "7", ... or "" = unknown
    bool is_can;
    int last_dtc_count;
    bool last_mil;
    bool bitmap40_asked;
    int replies;
    int64_t rate_from;
    int64_t now;                // the time of the call being handled
} elm327_t;

// saved_protocol: the digit save_protocol() stored last time, or "".
void elm_init(elm327_t *e, obd_state_t *state, const elm_transport_t *io, const char *saved_protocol);
void elm_forget_protocol(elm327_t *e);

void elm_on_link_up(elm327_t *e, int64_t now);
void elm_on_link_down(elm327_t *e);
void elm_on_reply(elm327_t *e, const uint8_t *raw, int len, int64_t now);
void elm_tick(elm327_t *e, int64_t now);

// Ask for Mode 04. Refused (false) unless the session is live and the engine
// is not running (RPM 0 or stale).
bool elm_request_clear(elm327_t *e, int64_t now);
