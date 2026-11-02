#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "elm327.h"
#include "obd/dtc.h"

// ATWS is a warm start: like ATZ but without the slow reset. ATAT1 is the
// default adaptive timing, set explicitly: ATAT2 (aggressive) is faster, but
// some Vgate adapters then miss replies (gps-laptimer forum).
static const char *INIT_SCRIPT[] = {"ATWS", "ATE0", "ATL0", "ATS0", "ATH0", "ATAT1", "ATI"};
#define N_INIT ((int)(sizeof(INIT_SCRIPT) / sizeof(INIT_SCRIPT[0])))

#define TIMEOUT_INIT    4000
#define TIMEOUT_DETECT  12000   // 0100 with ATSPA<n>: the saved protocol, then a search
#define TIMEOUT_SEARCH  25000   // 0100 with ATSP0: K-line slow inits come before CAN
// The display sits next to the adapter, so replies come in tens of ms; a
// short wait costs less RPM when one is lost.
#define TIMEOUT_POLL    1200
#define TIMEOUT_CLEAR   5000
// Mode 09 answers span several CAN frames and go without the count digit, so
// the ELM ends them on its own timeout.
#define TIMEOUT_INFO    1000
// ECU-off retries: every 5 s at first (a stall, a key turned off and on),
// then 15 s and 30 s for a long stop.
#define ECU_RETRY         5000
#define ECU_RETRY_LONG    15000
#define ECU_RETRY_PARKED  30000
#define DETECT_TIMEOUTS_TO_INIT 3
#define LIVE_FAILURES_TO_OFF    6
// ...and no value for this long. A count alone is not enough here: with a
// dozen jobs, PIDs the ECU lists but answers NO DATA can fail six in a row
// while RPM answers between them.
#define LIVE_SILENCE_TO_OFF     3000
#define TIMEOUTS_TO_RESTART     4   // one light retry while live, then two inits
#define ENGINE_RUNNING_RPM      300.0f

static void start_init(elm327_t *e);
static void send_detect(elm327_t *e);
static void start_lock(elm327_t *e);
static void start_bitmaps(elm327_t *e);
static void poll_next(elm327_t *e);

static bool is_can_protocol(const char *p)
{
    return p[0] && strchr("6789ABC", p[0]) != NULL;
}

void elm_init(elm327_t *e, obd_state_t *state, const elm_transport_t *io, const char *saved)
{
    memset(e, 0, sizeof(*e));
    e->state = state;
    e->io = *io;
    e->s = S_IDLE;
    e->is_can = true;
    e->last_dtc_count = -1;
    poller_init(&e->poller, state);
    if (saved && strlen(saved) == 1) {
        e->protocol[0] = saved[0];
        e->is_can = is_can_protocol(e->protocol);
    }
}

void elm_forget_protocol(elm327_t *e)
{
    e->protocol[0] = 0;
    e->is_can = true;
}

// ---- Sending ------------------------------------------------------------

static void send(elm327_t *e, const char *cmd, elm_kind_t kind, int timeout)
{
    snprintf(e->cmd, sizeof(e->cmd), "%s", cmd);
    size_t n = strlen(e->cmd);
    if (n == 0 || e->cmd[n - 1] != '\r') {
        if (n + 1 < sizeof(e->cmd)) {
            e->cmd[n++] = '\r';
            e->cmd[n] = 0;
        }
    }
    e->kind = kind;
    e->timeout = timeout;
    e->sent_at = e->now;
    e->in_flight = e->io.send(e->io.ctx, (const uint8_t *)e->cmd, (int)n);
    if (!e->in_flight) {
        e->kind = K_NONE;   // link went down; elm_on_link_down follows
    }
}

// ---- Link events --------------------------------------------------------

void elm_on_link_up(elm327_t *e, int64_t now)
{
    e->now = now;
    e->timeouts = 0;
    e->detect_timeouts = 0;
    e->ecu_off_misses = 0;
    e->reinit_pending = false;
    e->unsynced = false;
    e->weak = false;
    e->rate_from = now;
    e->replies = 0;
    poller_reset(&e->poller);
    start_init(e);
}

void elm_on_link_down(elm327_t *e)
{
    e->state->link_drops++;
    e->s = S_IDLE;
    e->in_flight = false;
    e->kind = K_NONE;
    obd_forget_values(e->state);
    if (e->state->clear == CLEAR_PENDING || e->state->clear == CLEAR_SENT) {
        e->state->clear = CLEAR_FAILED;
    }
}

// ---- Init script --------------------------------------------------------

static void start_init(elm327_t *e)
{
    e->s = S_INIT;
    e->state->phase = PHASE_INIT;
    e->step = 0;
    e->n_script = 0;
    for (int i = 0; i < N_INIT; i++) {
        snprintf(e->script[e->n_script++], sizeof(e->script[0]), "%s", INIT_SCRIPT[i]);
    }
    if (e->protocol_setting[0]) {
        snprintf(e->script[e->n_script++], sizeof(e->script[0]), "ATSP%s", e->protocol_setting);
        e->is_can = is_can_protocol(e->protocol_setting);
    } else if (e->protocol[0]) {
        // "A7" tries the saved protocol first and searches if it fails, so a
        // wrong saved value costs one search, not a stuck session.
        snprintf(e->script[e->n_script++], sizeof(e->script[0]), "ATSPA%s", e->protocol);
    } else {
        snprintf(e->script[e->n_script++], sizeof(e->script[0]), "ATSP0");
    }
    send(e, e->script[0], K_INIT, TIMEOUT_INIT);
}

static void on_init_reply(elm327_t *e, const elm_result_t *r)
{
    const char *cmd = e->script[e->step];
    if (strcmp(cmd, "ATI") == 0 && r->text[0] && strcmp(r->text, "OK") != 0) {
        snprintf(e->state->adapter, sizeof(e->state->adapter), "%s", r->text);
    }
    // A "?" to ATS0 or ATAT1 marks the cheap "v2.1" clones, which also garble
    // multi-PID requests: poll those one PID at a time.
    if (r->status == ELM_UNKNOWN_CMD && (strcmp(cmd, "ATS0") == 0 || strcmp(cmd, "ATAT1") == 0)) {
        e->weak = true;
    }
    e->step++;
    if (e->step < e->n_script) {
        send(e, e->script[e->step], K_INIT, TIMEOUT_INIT);
    } else {
        e->s = S_DETECT;
        e->failures = 0;
        send_detect(e);
    }
}

// ---- ECU detection ------------------------------------------------------

static void send_detect(elm327_t *e)
{
    if (e->s != S_ECU_OFF) {
        e->s = S_DETECT;
    }
    obd_clear_support(e->state);
    bool search = !e->protocol_setting[0] && !e->protocol[0];
    send(e, "0100", K_DETECT, search ? TIMEOUT_SEARCH : TIMEOUT_DETECT);
}

static void ecu_off(elm327_t *e)
{
    e->state->phase = PHASE_ECU_OFF;
    e->s = S_ECU_OFF;
    e->ecu_off_misses++;
    e->retry_at = e->now + (e->ecu_off_misses <= 3 ? ECU_RETRY
                            : e->ecu_off_misses <= 6 ? ECU_RETRY_LONG : ECU_RETRY_PARKED);
}

static void on_detect_reply(elm327_t *e, const elm_result_t *r)
{
    e->detect_timeouts = 0;
    int decoded = 0;
    for (int i = 0; i < r->n_msgs; i++) {
        decoded += obd_apply_mode01(e->state, r->msg[i], r->msg_len[i], e->now);
    }
    if (decoded > 0) {
        e->state->support_known = true;
        send(e, "ATDPN", K_DPN, TIMEOUT_INIT);
        return;
    }
    // No ECU: the ignition is off. The saved protocol is kept: with ATSPA the
    // adapter already searched the others before answering.
    ecu_off(e);
}

static void on_dpn_reply(elm327_t *e, const elm_result_t *r)
{
    // "A7" = found automatically, protocol 7. Save only a real protocol number:
    // a clone's "?" saved here would break every later ride.
    const char *t = r->text;
    if (strlen(t) == 2 && t[0] == 'A') {
        t++;
    }
    if (strlen(t) == 1 && strchr("123456789ABC", t[0]) != NULL) {
        if (t[0] != e->protocol[0]) {
            e->protocol[0] = t[0];
            e->protocol[1] = 0;
            e->is_can = is_can_protocol(e->protocol);
            if (e->io.save_protocol) {
                e->io.save_protocol(e->io.ctx, e->protocol);
            }
        }
    }
    snprintf(e->state->protocol, sizeof(e->state->protocol), "%s", t);
    start_lock(e);
}

// ---- Engine ECU lock (CAN) ----------------------------------------------

// A functional 0100 reaches every ECU, so the ABS unit may answer too.
// Address the engine ECU (7E0/7E8 or DA10F1/18DAF110) and only listen to it.
// If it does not answer there, go back to the defaults.
static void start_lock(elm327_t *e)
{
    e->state->ecu[0] = 0;
    char p = e->protocol[0];
    if (p == '6' || p == '8') {
        strcpy(e->lock[0], "ATSH7E0");
        strcpy(e->lock[1], "ATCRA7E8");
    } else if (p == '7' || p == '9') {
        strcpy(e->lock[0], "ATSHDA10F1");
        strcpy(e->lock[1], "ATCRA18DAF110");
    } else {
        start_bitmaps(e);   // unknown, not CAN, or a user CAN slot
        return;
    }
    e->n_lock = 2;
    e->lock_step = 0;
    send(e, e->lock[0], K_LOCK, TIMEOUT_INIT);
}

static void unlock(elm327_t *e)
{
    bool is29 = e->protocol[0] == '7' || e->protocol[0] == '9';
    strcpy(e->lock[0], "ATAR");
    strcpy(e->lock[1], is29 ? "ATSHDB33F1" : "ATSH7DF");
    e->n_lock = 2;
    e->lock_step = 0;
    send(e, e->lock[0], K_UNLOCK, TIMEOUT_INIT);
}

static void on_lock_reply(elm327_t *e, const elm_result_t *r)
{
    if (r->status == ELM_UNKNOWN_CMD) {
        unlock(e);   // an adapter without ATCRA
        return;
    }
    e->lock_step++;
    if (e->lock_step < e->n_lock) {
        send(e, e->lock[e->lock_step], K_LOCK, TIMEOUT_INIT);
    } else {
        send(e, "0100", K_LOCK_CHECK, TIMEOUT_POLL);
    }
}

static void on_lock_check_reply(elm327_t *e, const elm_result_t *r)
{
    bool engine = false;
    for (int i = 0; i < r->n_msgs; i++) {
        if (r->msg_len[i] >= 6 && r->msg[i][0] == 0x41 && r->msg[i][1] == 0x00) {
            engine = true;
        }
    }
    if (!engine) {
        unlock(e);
        return;
    }
    obd_clear_support(e->state);
    for (int i = 0; i < r->n_msgs; i++) {
        obd_apply_mode01(e->state, r->msg[i], r->msg_len[i], e->now);
    }
    e->state->support_known = true;
    snprintf(e->state->ecu, sizeof(e->state->ecu), "%s", e->lock[1] + 5);
    start_bitmaps(e);
}

static void on_unlock_reply(elm327_t *e)
{
    e->lock_step++;
    if (e->lock_step < e->n_lock) {
        send(e, e->lock[e->lock_step], K_UNLOCK, TIMEOUT_INIT);
    } else {
        start_bitmaps(e);
    }
}

// ---- Support bitmaps ----------------------------------------------------

static void go_live(elm327_t *e);

static void send_next_bitmap(elm327_t *e)
{
    if (e->n_bitmap_queue == 0) {
        go_live(e);
        return;
    }
    uint8_t pid = e->bitmap_queue[0];
    e->bitmap_queue[0] = e->bitmap_queue[1];
    e->n_bitmap_queue--;
    char cmd[8];
    snprintf(cmd, sizeof(cmd), "01%02X", pid);
    send(e, cmd, K_BITMAP, TIMEOUT_POLL);
}

static void start_bitmaps(elm327_t *e)
{
    snprintf(e->state->protocol, sizeof(e->state->protocol), "%s", e->protocol);
    e->bitmap40_asked = false;
    e->n_bitmap_queue = 0;
    if (obd_is_supported(e->state, PID_SUPPORTED_20)) {
        e->bitmap_queue[e->n_bitmap_queue++] = PID_SUPPORTED_20;
    }
    send_next_bitmap(e);
}

static void on_bitmap_reply(elm327_t *e, const elm_result_t *r)
{
    for (int i = 0; i < r->n_msgs; i++) {
        obd_apply_mode01(e->state, r->msg[i], r->msg_len[i], e->now);
    }
    // 0120 says whether 0140 exists.
    if (!e->bitmap40_asked && obd_is_supported(e->state, PID_SUPPORTED_40) && e->n_bitmap_queue < 2) {
        e->bitmap40_asked = true;
        e->bitmap_queue[e->n_bitmap_queue++] = PID_SUPPORTED_40;
    }
    send_next_bitmap(e);
}

// ---- Live polling -------------------------------------------------------

// Mode 09 once per live session: 0900 says which PIDs exist, then each of
// these that it lists. Only what the ECU lists is asked for.
static const uint8_t INFO_PIDS[] = {0x02, 0x04, 0x06};   // VIN, calibration IDs, CVNs
#define N_INFO ((int)(sizeof(INFO_PIDS) / sizeof(INFO_PIDS[0])))

static bool send_info(elm327_t *e)
{
    while (e->info_step <= N_INFO) {
        int step = e->info_step++;
        uint8_t pid = 0x00;
        if (step > 0) {
            pid = INFO_PIDS[step - 1];
            if (!(e->info_bitmap & (0x80 >> (pid - 1)))) {
                continue;
            }
        }
        e->info_pid = pid;
        char cmd[8];
        snprintf(cmd, sizeof(cmd), "09%02X", pid);
        send(e, cmd, K_INFO, TIMEOUT_INFO);
        return true;
    }
    e->state->info_read = true;
    return false;
}

static void info_done(elm327_t *e)
{
    e->info_step = N_INFO + 1;
    e->state->info_read = true;
}

static void on_info_reply(elm327_t *e, const elm_result_t *r)
{
    // The answer may follow a "response pending" (7F 09 78) in the same reply:
    // a CB500X's CVN does. Take the first message that answers this PID.
    const uint8_t *m = NULL;
    int n = 0;
    for (int i = 0; i < r->n_msgs && !m; i++) {
        if (r->msg_len[i] >= 2 && r->msg[i][0] == 0x49 && r->msg[i][1] == e->info_pid) {
            m = r->msg[i];
            n = r->msg_len[i];
        }
    }
    bool answered = m != NULL;
    if (e->info_pid == 0x00) {
        // No bitmap (NO DATA, 7F): the ECU has no Mode 09; skip the rest.
        e->info_bitmap = answered && n >= 3 ? m[2] : 0;
        if (!e->info_bitmap) info_done(e);
    } else if (answered) {
        obd_apply_mode09(e->state, m, n);
    }
    if (e->info_step > N_INFO) {
        e->state->info_read = true;   // that was the last one
    }
    poll_next(e);
}

static void go_live(elm327_t *e)
{
    e->s = S_LIVE;
    e->info_step = 0;
    e->info_bitmap = 0;
    e->failures = 0;
    e->last_decode_at = e->now;
    e->ecu_off_misses = 0;
    e->state->phase = PHASE_LIVE;
    if (e->weak) {
        poller_set_weak(&e->poller);
    }
    poller_rebuild(&e->poller);
    poll_next(e);
}

static void poll_next(elm327_t *e)
{
    if (e->state->clear == CLEAR_PENDING) {
        e->state->clear = CLEAR_SENT;
        send(e, "04", K_CLEAR, TIMEOUT_CLEAR);
        return;
    }
    if (e->info_step <= N_INFO && send_info(e)) {
        return;
    }
    int mode = poller_next_dtc_mode(&e->poller, e->now);
    if (mode > 0) {
        e->dtc_mode = mode;
        char cmd[12];
        snprintf(cmd, sizeof(cmd), "%02X", mode & 0xFF);
        send(e, cmd, K_DTC, TIMEOUT_POLL);
        return;
    }
    poll_job_t *job = poller_next(&e->poller, e->now);
    if (job) {
        e->job = job;
        send(e, job->cmd, K_POLL, TIMEOUT_POLL);
    }
}

// "13.8V" -> 13.8
static bool parse_volts(const char *text, float *out)
{
    const char *v = strchr(text, 'V');
    if (!v || v == text) {
        return false;
    }
    char *end;
    float f = strtof(text, &end);
    if (end == text) {
        return false;
    }
    *out = f;
    return true;
}

// After a 0101 reply: read the codes when the count or the MIL changes. Only
// with PID 01: without it the count stays 0, and acting on that would wipe the
// codes Mode 03 just read (they would vanish until the next read a minute
// later).
static void check_status(elm327_t *e)
{
    obd_state_t *s = e->state;
    if (!obd_is_supported(s, PID_STATUS)) {
        return;
    }
    if (s->dtc_count != e->last_dtc_count || s->mil != e->last_mil) {
        e->last_dtc_count = s->dtc_count;
        e->last_mil = s->mil;
        if (s->dtc_count > 0 || s->mil) {
            poller_request_dtcs(&e->poller);
        } else {
            s->n_dtcs = 0;
            s->n_pending = 0;
        }
    }
}

static void on_poll_reply(elm327_t *e, const elm_result_t *r)
{
    poll_job_t *job = e->job;
    bool is_at = job->is_at;
    int decoded = 0;
    if (is_at) {
        float v;
        if (parse_volts(r->text, &v)) {
            obd_state_set(e->state, PID_VOLTAGE, v, e->now);
            decoded = 1;
        }
    } else {
        for (int i = 0; i < r->n_msgs; i++) {
            decoded += obd_apply_mode01(e->state, r->msg[i], r->msg_len[i], e->now);
        }
    }
    poller_on_reply(&e->poller, job, decoded, r->status);   // may rebuild: job is stale after this
    e->job = NULL;

    // Only ECU requests say whether the ECU is there: the adapter answers
    // ATRV with the ignition off too, so it must not reset the count.
    if (!is_at) {
        if (decoded > 0 || r->status == ELM_STOPPED) {
            e->failures = 0;
            if (decoded > 0) {
                e->last_decode_at = e->now;
            }
        } else if (++e->failures >= LIVE_FAILURES_TO_OFF && e->now - e->last_decode_at >= LIVE_SILENCE_TO_OFF) {
            // Ignition off or the ECU went quiet: drop to slow retries.
            e->state->ecu_drops++;
            obd_forget_values(e->state);
            e->failures = 0;
            ecu_off(e);
            return;
        }
    }
    check_status(e);
    poll_next(e);
}

static void on_dtc_reply(elm327_t *e, const elm_result_t *r)
{
    obd_state_t *s = e->state;
    if (e->dtc_mode == DTC_MODE_STORED) {
        s->n_dtcs = dtc_decode_reply(r, e->dtc_mode, e->is_can, s->dtcs);
        s->dtcs_read = true;
    } else {
        s->n_pending = dtc_decode_reply(r, e->dtc_mode, e->is_can, s->pending);
    }
    poll_next(e);
}

static void on_clear_reply(elm327_t *e, const elm_result_t *r)
{
    bool ok = false;
    for (int i = 0; i < r->n_msgs; i++) {
        if (r->msg_len[i] >= 1 && r->msg[i][0] == 0x44) {
            ok = true;
        }
    }
    e->state->clear = ok ? CLEAR_OK : CLEAR_FAILED;
    if (ok) {
        e->state->n_dtcs = 0;
        e->state->n_pending = 0;
        e->state->mil = false;
    }
    poller_request_dtcs(&e->poller);   // read them back either way
    poll_next(e);
}

bool elm_request_clear(elm327_t *e, int64_t now)
{
    float rpm;
    if (e->s != S_LIVE) {
        return false;
    }
    if (obd_value(e->state, PID_RPM, now, &rpm) && rpm >= ENGINE_RUNNING_RPM) {
        return false;
    }
    e->state->clear = CLEAR_PENDING;
    e->now = now;
    if (!e->in_flight) {
        poll_next(e);
    }
    return true;
}

// ---- Replies and timeouts -----------------------------------------------

static bool cmd_is(const elm327_t *e, const char *at)
{
    size_t n = strlen(at);
    return strncmp(e->cmd, at, n) == 0 && (e->cmd[n] == '\r' || e->cmd[n] == 0);
}

// Text that only some commands answer: "OK" answers AT setters, "ELM327 v2.3"
// a reset or ATI, and AT commands run at once, so never end STOPPED. After a
// timeout the old command's late answer can arrive while the next one waits;
// taken, every later reply lands one command late. A test ride lost a minute so:
// ATWS took a late STOPPED, then ATE0 got "ELM327", ATI "OK" and 0100 "OK",
// and the ECU looked off. Checked only after a timeout, so a clone's odd
// answers don't matter while replies are known to line up.
//
// The one exception is the adapter's name: only a reset or ATI asks for it,
// but an adapter that has just powered up sends its banner unasked. Seen at
// key-on: ATWS took the banner, the real "ATWS ELM327 v2.3" landed on ATE0,
// and 0100 got "OK". So a name answering anything else is stray, always.
static bool is_stray_text(const elm327_t *e, const elm_result_t *r)
{
    bool id = strstr(r->text, "ELM") != NULL;
    bool asks_id = cmd_is(e, "ATWS") || cmd_is(e, "ATZ") || cmd_is(e, "ATI");
    if (id && !asks_id) {
        return true;
    }
    if (!e->unsynced) {
        return false;
    }
    bool ok = strcmp(r->text, "OK") == 0;
    if (e->cmd[0] != 'A') {
        return ok;   // an OBD request
    }
    if (r->status == ELM_STOPPED) {
        return true;
    }
    if (asks_id) {
        return ok;
    }
    return ok && (cmd_is(e, "ATRV") || cmd_is(e, "ATDPN"));
}

// Whether a reply cannot be the answer to the command in flight: text another
// command gives (above), or an OBD answer (41.., 43.., 7F..) to an AT command,
// or to another mode or PID. NO DATA and ? carry no identity: taken as they come.
static bool is_stray(const elm327_t *e, const elm_result_t *r)
{
    if (strlen(e->cmd) < 2) {
        return false;
    }
    if (r->n_msgs == 0) {
        return is_stray_text(e, r);
    }
    const uint8_t *m = r->msg[0];
    if (r->msg_len[0] < 2) {
        return false;   // "A7" from ATDPN decodes to one byte
    }
    int svc = m[0];
    if (svc != 0x7F && (svc < 0x41 || svc > 0x4A)) {
        return false;
    }
    if (e->cmd[0] == 'A') {
        return true;    // an OBD answer to an AT command
    }
    int mode = (hex_nibble(e->cmd[0]) << 4) | hex_nibble(e->cmd[1]);
    if (svc == 0x7F) {
        return m[1] != mode;   // a negative response names the mode
    }
    if (svc != mode + 0x40) {
        return true;
    }
    if (mode != 1) {
        return false;
    }
    // Mode 01: the reply's first PID must be one we asked for: the hex pairs
    // after "01", before the CR and any count digit.
    size_t n = strlen(e->cmd);
    for (size_t i = 2; i + 2 < n; i += 2) {
        int a = hex_nibble(e->cmd[i]), b = hex_nibble(e->cmd[i + 1]);
        if (a >= 0 && b >= 0 && ((a << 4) | b) == m[1]) {
            return false;
        }
    }
    return true;
}

void elm_on_reply(elm327_t *e, const uint8_t *raw, int len, int64_t now)
{
    if (!e->in_flight) {
        return;   // a late reply to a command that already timed out
    }
    e->now = now;
    bool obd = e->kind == K_DETECT || e->kind == K_BITMAP || e->kind == K_DTC || e->kind == K_LOCK_CHECK
               || e->kind == K_CLEAR || e->kind == K_INFO || (e->kind == K_POLL && e->job && !e->job->is_at);
    static elm_result_t r;   // one session per app; keeps 600 bytes off the stack
    elm_parse(raw, len, obd, &r);
    if (is_stray(e, &r)) {
        // The answer to a command that timed out, arriving after the next one
        // went out. Taking it would shift every later reply onto the wrong
        // command, so drop it and keep waiting for ours.
        obd_set_error(e->state, "stray reply");
        return;
    }
    e->in_flight = false;
    e->timeouts = 0;
    if (r.n_msgs > 0 || strstr(r.text, "ELM")) {
        e->unsynced = false;   // an answer that names its command: lined up again
    }
    e->replies++;
    e->state->last_reply_at = now;
    if (r.status != ELM_OK) {
        obd_set_error(e->state, r.text[0] ? r.text : "empty reply");
    }
    if (e->reinit_pending) {
        e->reinit_pending = false;
        start_init(e);
        return;
    }
    switch (e->kind) {
    case K_INIT: on_init_reply(e, &r); break;
    case K_DETECT: on_detect_reply(e, &r); break;
    case K_DPN: on_dpn_reply(e, &r); break;
    case K_BITMAP: on_bitmap_reply(e, &r); break;
    case K_LOCK: on_lock_reply(e, &r); break;
    case K_LOCK_CHECK: on_lock_check_reply(e, &r); break;
    case K_UNLOCK: on_unlock_reply(e); break;
    case K_DTC: on_dtc_reply(e, &r); break;
    case K_CLEAR: on_clear_reply(e, &r); break;
    case K_INFO: on_info_reply(e, &r); break;
    case K_POLL: on_poll_reply(e, &r); break;
    case K_NONE: break;
    }
}

static void on_timeout(elm327_t *e)
{
    // A timed-out reset heard nothing it could take for 4 s: anything late has
    // come and gone, so take what follows as it comes (a clone answering ATWS
    // with OK must not loop).
    e->unsynced = e->kind != K_INIT;
    e->in_flight = false;
    e->state->timeouts++;
    obd_set_error(e->state, "timeout");
    if (e->io.discard_partial) {
        e->io.discard_partial(e->io.ctx);
    }
    if (e->reinit_pending) {
        e->reinit_pending = false;
        start_init(e);
        return;
    }
    if (e->kind == K_DETECT) {
        // Usually no ECU on any protocol yet. Only several in a row point at a
        // stuck adapter, worth an init.
        if (++e->detect_timeouts >= DETECT_TIMEOUTS_TO_INIT) {
            e->detect_timeouts = 0;
            e->state->reinits++;
            start_init(e);
        } else {
            ecu_off(e);
        }
        return;
    }
    if (e->kind == K_INFO) {
        // Nice to have, never worth a restart: a lost bitmap skips Mode 09,
        // a lost item moves on to the next.
        if (e->info_pid == 0x00 || e->info_step > N_INFO) info_done(e);
        poll_next(e);
        return;
    }
    if (e->kind == K_DTC) {
        poller_dtc_timed_out(&e->poller);
    }
    if (e->kind == K_CLEAR) {
        e->state->clear = CLEAR_FAILED;
    }
    if (e->kind == K_POLL) {
        e->job = NULL;
    }
    if (++e->timeouts >= TIMEOUTS_TO_RESTART) {
        e->timeouts = 0;
        e->io.restart(e->io.ctx);
        return;
    }
    if (e->s == S_LIVE && e->timeouts == 1) {
        // One lost or late reply while polling: go on with the next request.
        // If the answer still comes, is_stray() drops it.
        poll_next(e);
        return;
    }
    // A stuck ELM: start the session again on the same link.
    e->state->reinits++;
    start_init(e);
}

void elm_tick(elm327_t *e, int64_t now)
{
    e->now = now;
    if (now - e->rate_from >= 2000) {
        e->state->replies_per_sec = e->replies * 1000.0f / (float)(now - e->rate_from);
        e->replies = 0;
        e->rate_from = now;
    }
    if (e->s == S_IDLE) {
        return;
    }
    if (e->in_flight) {
        if (now - e->sent_at > e->timeout) {
            on_timeout(e);
        }
        return;
    }
    if (e->s == S_ECU_OFF && now >= e->retry_at) {
        send_detect(e);
    } else if (e->s == S_LIVE) {
        poll_next(e);   // nothing was due at the last reply
    }
}
