/*
 * Decides which request goes out next. The display polls as fast as the adapter answers: RPM and speed lead (every 100 ms), the
 * throttle group follows (250 ms), temperatures every second, the rest every
 * few seconds or 30 s. The most overdue job always goes next, so the fast
 * groups interleave with the slow ones. When the ECU accepts multi-PID
 * requests, PIDs are grouped so each reply still fits one CAN frame (at most
 * 6 bytes of PID + data after 0x41); then the ELM can stop after one reply
 * ("1" suffix) instead of waiting out its timeout.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "obd_state.h"
#include "elm/elm_reply.h"

#define POLL_MAX_JOBS 16

typedef struct {
    uint8_t pids[4];
    int n;
    int period;         // ms
    int64_t last_run;
    char cmd[20];       // with its CR
    bool is_at;         // an AT command (ATRV), not a Mode 01 request
} poll_job_t;

typedef struct {
    obd_state_t *state;
    poll_job_t jobs[POLL_MAX_JOBS];
    int n_jobs;
    bool multi;
    bool use_count;     // some ELM327 clones do not understand the reply-count digit
    int unknowns;       // "?" replies to Mode 01 requests in a row
    int multi_failures;
    bool multi_confirmed;
    int dtc_next;       // index into the DTC modes; 2 = nothing queued
    bool dtc_retried;
    int64_t last_dtc_poll;
} poller_t;

void poller_init(poller_t *p, obd_state_t *state);
// A new connection starts from the defaults.
void poller_reset(poller_t *p);
// Rebuild the job list from what the ECU supports.
void poller_rebuild(poller_t *p);
// A clone that refused ATS0 or ATAT1: one PID per request, no count digit.
void poller_set_weak(poller_t *p);
// Queue a fault-code read; it goes out before the next poll.
void poller_request_dtcs(poller_t *p);
void poller_dtc_timed_out(poller_t *p);
// The DTC mode to send next, or -1.
int poller_next_dtc_mode(poller_t *p, int64_t now);
// The most overdue job, or NULL if nothing is due yet.
poll_job_t *poller_next(poller_t *p, int64_t now);
// Account for a reply: decoded = PIDs it carried, status = its parse status.
void poller_on_reply(poller_t *p, poll_job_t *job, int decoded, elm_status_t status);
