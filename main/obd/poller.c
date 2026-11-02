#include <stdio.h>
#include <string.h>
#include "poller.h"
#include "dtc.h"

typedef struct {
    uint8_t pids[3];
    int n;
    int period;
} group_t;

// Groups for multi-PID requests. Each fits one CAN frame (PID + data <= 6 bytes).
// Periods: with a Vgate iCar Pro a reply takes ~30 ms (median; p95 50), so
// the link carries ~30 requests a second. RPM+speed take what the others
// leave (~16-20 Hz); a slower adapter just stretches every period alike.
static const group_t GROUPS[] = {
    {{PID_RPM, PID_SPEED}, 2, 50},                        // 3 + 2
    {{PID_THROTTLE, PID_LOAD, PID_MAP}, 3, 150},          // 2 + 2 + 2
    {{PID_COOLANT, PID_IAT, PID_ADVANCE}, 3, 1000},       // 2 + 2 + 2
    // Some ECUs (the CB500X's) answer 06 with two bytes (bank 1 and bank 3: "41 06 80 00 03 04 00"),
    // which a reply can't tell from one: the trim goes last, where its extra
    // byte is just left over. With the trim first, PID 03 was never read.
    {{PID_FUEL_STATUS, PID_STFT1}, 2, 2000},              // 3 + 2 or 3
    {{PID_VOLTAGE}, 1, 2000},
    {{PID_MIL_DIST, PID_OBD_STD}, 2, 30000},              // 3 + 2
    {{PID_MONITOR}, 1, 30000},
    {{PID_SEC_AIR}, 1, 30000},
    {{PID_STATUS}, 1, 30000},
};
#define N_GROUPS ((int)(sizeof(GROUPS) / sizeof(GROUPS[0])))

#define ATRV_PERIOD 2000
// A job counts as due this early: the session ticks every 20 ms.
#define SLACK 20
// Without PID 01 (MIL and code count) there is no cheap way to notice a new
// fault code, so read the codes themselves this often. Many bikes' ECUs are such.
#define DTC_POLL_PERIOD 60000
// Multi-PID requests that fail this many times in a row switch to single.
#define MULTI_FAILURES 3

static const int DTC_MODES[] = {DTC_MODE_STORED, DTC_MODE_PENDING};
#define N_DTC_MODES 2

void poller_init(poller_t *p, obd_state_t *state)
{
    memset(p, 0, sizeof(*p));
    p->state = state;
    poller_reset(p);
}

void poller_reset(poller_t *p)
{
    p->multi = true;
    p->use_count = true;
    p->unknowns = 0;
    p->multi_failures = 0;
    p->multi_confirmed = false;
    p->dtc_next = N_DTC_MODES;
    p->dtc_retried = false;
    p->last_dtc_poll = -1;
    p->n_jobs = 0;
}

void poller_set_weak(poller_t *p)
{
    p->multi = false;
    p->use_count = false;
}

// "01" + PIDs + "1" + CR: the trailing digit tells the ELM to return after one
// reply instead of waiting for more ECUs. Dropped if the adapter answers "?".
static void add_job(poller_t *p, const uint8_t *pids, int n, int period)
{
    if (p->n_jobs >= POLL_MAX_JOBS || n == 0) {
        return;
    }
    poll_job_t *j = &p->jobs[p->n_jobs++];
    memset(j, 0, sizeof(*j));
    memcpy(j->pids, pids, n);
    j->n = n;
    j->period = period;
    j->last_run = -1000000;
    int o = snprintf(j->cmd, sizeof(j->cmd), "01");
    for (int i = 0; i < n; i++) {
        o += snprintf(j->cmd + o, sizeof(j->cmd) - o, "%02X", pids[i]);
    }
    snprintf(j->cmd + o, sizeof(j->cmd) - o, p->use_count ? "1\r" : "\r");
}

void poller_rebuild(poller_t *p)
{
    p->n_jobs = 0;
    for (int g = 0; g < N_GROUPS; g++) {
        const group_t *grp = &GROUPS[g];
        if (p->multi) {
            uint8_t pids[3];
            int n = 0;
            for (int i = 0; i < grp->n; i++) {
                if (obd_is_supported(p->state, grp->pids[i])) pids[n++] = grp->pids[i];
            }
            add_job(p, pids, n, grp->period);
        } else {
            for (int i = 0; i < grp->n; i++) {
                if (obd_is_supported(p->state, grp->pids[i])) add_job(p, &grp->pids[i], 1, grp->period);
            }
        }
    }
    if (!obd_is_supported(p->state, PID_VOLTAGE) && p->n_jobs < POLL_MAX_JOBS) {
        // The ELM measures the supply on pin 16: the battery voltage.
        poll_job_t *j = &p->jobs[p->n_jobs++];
        memset(j, 0, sizeof(*j));
        j->pids[0] = PID_VOLTAGE;
        j->n = 1;
        j->period = ATRV_PERIOD;
        j->last_run = -1000000;
        j->is_at = true;
        strcpy(j->cmd, "ATRV\r");
    }
    p->state->multi_pid = p->multi;
}

void poller_request_dtcs(poller_t *p)
{
    if (p->dtc_next >= N_DTC_MODES) {
        p->dtc_next = 0;
        p->dtc_retried = false;
    }
}

void poller_dtc_timed_out(poller_t *p)
{
    if (!p->dtc_retried && p->dtc_next > 0) {
        p->dtc_next--;
        p->dtc_retried = true;
    }
}

int poller_next_dtc_mode(poller_t *p, int64_t now)
{
    if (p->state->support_known && !obd_is_supported(p->state, PID_STATUS)
        && (p->last_dtc_poll < 0 || now - p->last_dtc_poll >= DTC_POLL_PERIOD)) {
        p->last_dtc_poll = now;
        poller_request_dtcs(p);
    }
    if (p->dtc_next >= N_DTC_MODES) {
        return -1;
    }
    return DTC_MODES[p->dtc_next++];
}

poll_job_t *poller_next(poller_t *p, int64_t now)
{
    poll_job_t *best = NULL;
    int64_t best_late = -1;
    for (int i = 0; i < p->n_jobs; i++) {
        poll_job_t *j = &p->jobs[i];
        int64_t late = now - j->last_run - j->period + SLACK;
        if (late >= 0 && late > best_late) {
            best = j;
            best_late = late;
        }
    }
    if (best) {
        best->last_run = now;
    }
    return best;
}

void poller_on_reply(poller_t *p, poll_job_t *job, int decoded, elm_status_t status)
{
    // Two in a row: one "?" can be a garbled command, and dropping the digit
    // makes every later request wait out the ELM's timeout.
    if (!job->is_at) {
        p->unknowns = status == ELM_UNKNOWN_CMD ? p->unknowns + 1 : 0;
    }
    if (p->unknowns >= 2 && p->use_count) {
        p->use_count = false;
        poller_rebuild(p);
        return;
    }
    if (!p->multi || job->is_at || job->n < 2) {
        return;
    }
    // A partial reply (some ECUs answer only the first PID) counts as a failure.
    if (decoded >= job->n) {
        p->multi_failures = 0;
        p->multi_confirmed = true;
        return;
    }
    p->multi_failures++;
    if (p->multi_failures >= MULTI_FAILURES && !p->multi_confirmed) {
        p->multi = false;
        poller_rebuild(p);
    }
}
