/*
 * Host tests for the protocol core. Transcripts in test/fixtures are
 * played back through the real session: when
 * the session sends a request, the fake adapter answers with the next reply
 * the transcript has for it. Requests are matched without spaces and without
 * the reply-count digit. A request the transcript lacks gets what an adapter
 * would most likely say: "OK" for AT commands, "NO DATA" for the ECU.
 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "elm/elm327.h"
#include "elm/elm_reply.h"
#include "obd/alerts.h"
#include "obd/board_guard.h"
#include "bike/bike.h"
#include "obd/dtc.h"
#include "obd/fuel.h"
#include "obd/gear.h"
#include "demo/demo.h"
#include "obd/obd_state.h"
#include "obd/pids.h"
#include "obd/ride_summary.h"
#include "obd/trip.h"
#include "sensors/motion.h"
#include "sensors/held_peak.h"
#include "sensors/rtc_regs.h"

#include "test_util.h"

int s_failures, s_checks;

// ---- Transcript replay ------------------------------------------------------

#define MAX_ENTRIES 128

typedef struct {
    char key[32];
    char reply[160];
    bool used;
} entry_t;

typedef struct {
    entry_t e[MAX_ENTRIES];
    int n;
} script_t;

typedef struct {
    char last[32];
    int sent;
    int restarts;
} fake_io_t;

static void make_key(const char *cmd, char *out, size_t cap)
{
    size_t n = 0;
    for (const char *c = cmd; *c && n + 1 < cap; c++) {
        if (*c != ' ' && *c != '\r') out[n++] = (char)toupper((unsigned char)*c);
    }
    out[n] = 0;
    if (strncmp(out, "AT", 2) != 0 && n % 2 == 1) {
        out[n - 1] = 0;   // the reply-count digit
    }
}

static void load_script(const char *name, script_t *s)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("  cannot open %s\n", path);
        exit(2);
    }
    memset(s, 0, sizeof(*s));
    char line[256];
    entry_t *cur = NULL;
    while (fgets(line, sizeof(line), f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#') continue;
        if (line[0] == '>') {
            cur = &s->e[s->n++];
            make_key(line + 1, cur->key, sizeof(cur->key));
        } else if (cur && line[0]) {
            strncat(cur->reply, line, sizeof(cur->reply) - strlen(cur->reply) - 2);
            strcat(cur->reply, "\r");
        }
    }
    fclose(f);
}

static const char *find_reply(script_t *s, const char *key)
{
    entry_t *last = NULL;
    for (int i = 0; i < s->n; i++) {
        if (strcmp(s->e[i].key, key) == 0) {
            if (!s->e[i].used) {
                s->e[i].used = true;
                return s->e[i].reply;
            }
            last = &s->e[i];
        }
    }
    if (last) return last->reply;   // the last answer repeats
    if (strcmp(key, "ATI") == 0 || strcmp(key, "ATWS") == 0) return "ELM327 v1.5\r";
    if (strcmp(key, "ATDPN") == 0) return "A7\r";
    if (strncmp(key, "AT", 2) == 0) return "OK\r";
    return "NO DATA\r";
}

static bool fake_send(void *ctx, const uint8_t *data, int len)
{
    fake_io_t *io = ctx;
    int n = len < (int)sizeof(io->last) - 1 ? len : (int)sizeof(io->last) - 1;
    memcpy(io->last, data, n);
    io->last[n] = 0;
    io->sent++;
    return true;
}

static void fake_restart(void *ctx) { ((fake_io_t *)ctx)->restarts++; }

typedef struct {
    elm327_t elm;
    obd_state_t state;
    fake_io_t io;
    int64_t t;
} rig_t;

static void rig_start(rig_t *r)
{
    memset(r, 0, sizeof(*r));
    obd_state_init(&r->state);
    elm_transport_t io = {.send = fake_send, .restart = fake_restart, .ctx = &r->io};
    elm_init(&r->elm, &r->state, &io, "");
    r->t = 1000;
    elm_on_link_up(&r->elm, r->t);
}

static void reply_text(rig_t *r, const char *text)
{
    elm_on_reply(&r->elm, (const uint8_t *)text, (int)strlen(text), r->t);
}

static void replay(rig_t *r, const char *fixture, int steps)
{
    static script_t s;
    load_script(fixture, &s);
    rig_start(r);
    for (int i = 0; i < steps; i++) {
        char key[32];
        make_key(r->io.last, key, sizeof(key));
        int before = r->io.sent;
        if (getenv("TRACE")) printf("    > %-14s phase %d dtcs %d/%d err %s\n", key, r->state.phase, r->state.n_dtcs, r->state.n_pending, r->state.last_error);
        reply_text(r, find_reply(&s, key));
        r->t += 60;
        elm_tick(&r->elm, r->t);
        if (r->io.sent == before) {
            r->t += 5000;   // nothing sent (waiting to retry): let time pass
            elm_tick(&r->elm, r->t);
        }
    }
}

static float fresh(rig_t *r, uint8_t pid)
{
    float v = NAN;
    obd_fresh(&r->state, pid, r->t, 60000, &v);
    return v;
}

// ---- Tests --------------------------------------------------------------

static void test_parse(void)
{
    elm_result_t res;
    const char *a = "SEARCHING...\r4100BE3EA813\r\r";
    elm_parse((const uint8_t *)a, (int)strlen(a), true, &res);
    CHECK(res.status == ELM_OK);
    CHECK(res.n_msgs == 1);
    CHECK(res.msg_len[0] == 6 && res.msg[0][0] == 0x41 && res.msg[0][5] == 0x13);

    const char *spaced = "41 0C 1A F8\r";
    elm_parse((const uint8_t *)spaced, (int)strlen(spaced), true, &res);
    CHECK(res.n_msgs == 1 && res.msg_len[0] == 4 && res.msg[0][3] == 0xF8);

    const char *nodata = "NO DATA\r";
    elm_parse((const uint8_t *)nodata, (int)strlen(nodata), true, &res);
    CHECK(res.status == ELM_NO_DATA && res.n_msgs == 0);

    // ISO-TP multi-frame: a 0x0E-byte Mode 03 reply split over 3 frames.
    const char *mf2 = "00A\r0:43040133017102\r1:01030000000000\r";
    elm_parse((const uint8_t *)mf2, (int)strlen(mf2), true, &res);
    CHECK(res.n_msgs == 1);
    CHECK(res.msg_len[0] == 10);
    char codes[DTC_MAX][DTC_LEN];
    int n = dtc_decode_reply(&res, DTC_MODE_STORED, true, codes);
    CHECK(n == 4);
    if (n == 4) {
        CHECK_STR(codes[0], "P0133");
        CHECK_STR(codes[1], "P0171");
        CHECK_STR(codes[2], "P0201");
        CHECK_STR(codes[3], "P0300");
    }

    const char *at = "13.8V\r";
    elm_parse((const uint8_t *)at, (int)strlen(at), false, &res);
    CHECK(res.status == ELM_OK);
    CHECK_STR(res.text, "13.8V");

    const char *q = "?\r";
    elm_parse((const uint8_t *)q, (int)strlen(q), false, &res);
    CHECK(res.status == ELM_UNKNOWN_CMD);
}

static int s_replies;
static void count_reply(void *ctx, const uint8_t *reply, int len) { s_replies++; }

static void test_assembler(void)
{
    elm_assembler_t a;
    elm_assembler_reset(&a);
    s_replies = 0;
    const char *p1 = "410C1A";
    const char *p2 = "F8\r\r>41";
    const char *p3 = "0D00\r>";
    elm_assembler_feed(&a, (const uint8_t *)p1, strlen(p1), count_reply, NULL);
    CHECK(s_replies == 0);
    elm_assembler_feed(&a, (const uint8_t *)p2, strlen(p2), count_reply, NULL);
    CHECK(s_replies == 1);
    elm_assembler_feed(&a, (const uint8_t *)p3, strlen(p3), count_reply, NULL);
    CHECK(s_replies == 2);
}

// The CB500X's ECU answers 06 with two bytes. Asked as 03 then 06, both decode.
// Asked the old way round, the trim decodes and the misread rest must not
// touch the support bitmap.
static void test_fuel_status_decode(void)
{
    obd_state_t s;
    float v;
    obd_state_init(&s);
    const uint8_t now_order[] = {0x41, 0x03, 0x04, 0x00, 0x06, 0x80, 0x00};
    CHECK(obd_apply_mode01(&s, now_order, sizeof(now_order), 1000) == 2);
    CHECK(obd_value(&s, PID_FUEL_STATUS, 1000, &v) && s.raw[PID_FUEL_STATUS][0] == 0x04);
    CHECK(obd_value(&s, PID_STFT1, 1000, &v) && fabsf(v) < 0.01f);
    const uint8_t closed[] = {0x41, 0x03, 0x02, 0x00, 0x06, 0x6D, 0x00};
    obd_apply_mode01(&s, closed, sizeof(closed), 2000);
    CHECK(s.raw[PID_FUEL_STATUS][0] == 0x02);
    CHECK(obd_value(&s, PID_STFT1, 2000, &v) && fabsf(v - -14.84f) < 0.05f);

    obd_state_init(&s);
    const uint8_t support[] = {0x41, 0x00, 0x7C, 0x3E, 0xC0, 0x11};
    obd_apply_mode01(&s, support, sizeof(support), 1000);
    CHECK(obd_is_supported(&s, PID_FUEL_STATUS));
    const uint8_t old_order[] = {0x41, 0x06, 0x80, 0x00, 0x03, 0x04, 0x00, 0x00, 0x00};
    CHECK(obd_apply_mode01(&s, old_order, sizeof(old_order), 2000) == 1);
    CHECK(obd_is_supported(&s, PID_FUEL_STATUS));   // the 00 read as a bitmap changed nothing
    CHECK(obd_is_supported(&s, PID_RPM));
}

static void test_pids(void)
{
    const uint8_t rpm[] = {0x12, 0xD4};
    CHECK_NEAR(pid_decode(PID_RPM, rpm), 1205.0, 0.01);
    // PID 1C: named up to 35 (the public J1979 list); the CB500X's 36 isn't.
    CHECK(strcmp(pid_obd_std_text(33), "HD EOBD-IV") == 0);
    CHECK(pid_obd_std_text(16) == NULL);
    CHECK(pid_obd_std_text(36) == NULL);
    const uint8_t cool[] = {0x73};
    CHECK_NEAR(pid_decode(PID_COOLANT, cool), 75.0, 0.01);
    const uint8_t adv[] = {0x90};
    CHECK_NEAR(pid_decode(PID_ADVANCE, adv), 8.0, 0.01);
    const uint8_t stft[] = {0x80};
    CHECK_NEAR(pid_decode(PID_STFT1, stft), 0.0, 0.01);
    const uint8_t thr[] = {0xFF};
    CHECK_NEAR(pid_decode(PID_THROTTLE, thr), 100.0, 0.01);

    obd_state_t s;
    obd_state_init(&s);
    const uint8_t bitmap[] = {0x41, 0x00, 0x7C, 0x3E, 0xC0, 0x11};
    CHECK(obd_apply_mode01(&s, bitmap, sizeof(bitmap), 0) == 1);
    const uint8_t want[] = {0x02, 0x03, 0x04, 0x05, 0x06, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x11, 0x12, 0x1C, 0x20};
    for (size_t i = 0; i < sizeof(want); i++) CHECK(obd_is_supported(&s, want[i]));
    CHECK(!obd_is_supported(&s, PID_STATUS));
    char hex[9];
    obd_support_hex(&s, 0x00, hex);
    CHECK_STR(hex, "7C3EC011");
}

static void test_replay_live(void)
{
    rig_t r;
    replay(&r, "cb500x-live.txt", 80);
    CHECK(r.state.phase == PHASE_LIVE);
    CHECK_STR(r.state.protocol, "7");
    CHECK_STR(r.state.ecu, "18DAF110");
    CHECK_STR(r.state.adapter, "ELM327 v2.3");
    CHECK(r.state.multi_pid);
    CHECK_NEAR(fresh(&r, PID_RPM), 1206.0, 2.0);
    CHECK_NEAR(fresh(&r, PID_SPEED), 0.0, 0.01);
    CHECK_NEAR(fresh(&r, PID_MAP), 92.0, 0.01);
    CHECK_NEAR(fresh(&r, PID_COOLANT), 75.0, 0.01);
    CHECK_NEAR(fresh(&r, PID_IAT), 27.0, 0.01);
    CHECK_NEAR(fresh(&r, PID_ADVANCE), 8.0, 0.01);
    CHECK_NEAR(fresh(&r, PID_VOLTAGE), 14.7, 0.01);
    CHECK_NEAR(fresh(&r, PID_OBD_STD), 6.0, 0.01);
    CHECK(r.state.raw[PID_FUEL_STATUS][0] == 0x02);
    CHECK(r.state.dtcs_read && r.state.n_dtcs == 0 && r.state.n_pending == 0);
    CHECK(r.io.restarts == 0);
    // RPM leads: in a steady stream, at least a third of requests are RPM.
    int rpm_requests = 0;
    static script_t s;
    load_script("cb500x-live.txt", &s);
    for (int i = 0; i < 60; i++) {
        char key[32];
        make_key(r.io.last, key, sizeof(key));
        if (strcmp(key, "010C0D") == 0) rpm_requests++;
        reply_text(&r, find_reply(&s, key));
        r.t += 40;
        elm_tick(&r.elm, r.t);
    }
    CHECK(rpm_requests >= 20);
}

static void test_mode09(void)
{
    // Listed in 0900: VIN, calibration ID and CVN, read once after going live.
    rig_t r;
    replay(&r, "cb500x-mode09.txt", 40);
    CHECK(r.state.phase == PHASE_LIVE);
    CHECK(r.state.info_read);
    CHECK_STR(r.state.vin, "TESTVIN0123456789");
    CHECK_STR(r.state.cal_id[0], "38770-MKP-D020");
    CHECK_STR(r.state.cal_id[1], "");
    CHECK_STR(r.state.cvn[0], "1A2B3C4D");
    CHECK(r.io.restarts == 0);
    CHECK_NEAR(fresh(&r, PID_RPM), 1206.0, 2.0);   // polling carried on

    // A real CB500X: no VIN (all zeros), its calibration ID, and
    // the CVN after a "response pending" in the same reply.
    replay(&r, "cb500x-mode09-real.txt", 40);
    CHECK(r.state.info_read);
    CHECK_STR(r.state.vin, "");
    CHECK_STR(r.state.cal_id[0], "0104301801");
    CHECK_STR(r.state.cvn[0], "CA80C980");
    CHECK(r.io.restarts == 0);

    // No Mode 09 (the live transcript answers 0900 with NO DATA): skipped,
    // nothing else asked, polling goes on as before.
    replay(&r, "cb500x-live.txt", 40);
    CHECK(r.state.info_read);
    CHECK_STR(r.state.vin, "");
    CHECK(r.io.restarts == 0);
    CHECK_NEAR(fresh(&r, PID_RPM), 1206.0, 2.0);
}

static void test_replay_fault(void)
{
    rig_t r;
    replay(&r, "cb500x-fault.txt", 40);
    CHECK(r.state.phase == PHASE_LIVE);
    CHECK(r.state.n_dtcs == 1);
    if (r.state.n_dtcs == 1) CHECK_STR(r.state.dtcs[0], "P0133");
    CHECK(r.state.n_pending == 1);
    if (r.state.n_pending == 1) CHECK_STR(r.state.pending[0], "P0171");
    CHECK_STR(dtc_describe("P0133"), "O2 sensor slow response (B1S1)");
    CHECK_STR(dtc_describe("P1234"), "Manufacturer powertrain code");
}

static void test_replay_single_pid(void)
{
    rig_t r;
    replay(&r, "cb500x-single-pid.txt", 80);
    CHECK(r.state.phase == PHASE_LIVE);
    CHECK(!r.state.multi_pid);
    CHECK_STR(r.state.ecu, "");   // the lock was refused
    CHECK_NEAR(fresh(&r, PID_RPM), 1200.0, 0.01);
    CHECK_NEAR(fresh(&r, PID_COOLANT), 34.0, 0.01);
    CHECK_NEAR(fresh(&r, PID_VOLTAGE), 14.6, 0.01);
}

static void test_replay_ignition_off(void)
{
    rig_t r;
    replay(&r, "synthetic-ignition-off.txt", 20);
    CHECK(r.state.phase == PHASE_ECU_OFF);
    CHECK_STR(r.io.last, "0100\r");   // still retrying
}

static void test_stray_reply(void)
{
    static script_t s;
    rig_t r;
    replay(&r, "cb500x-live.txt", 60);
    CHECK(r.state.phase == PHASE_LIVE);
    // Force a known request in flight, then deliver the answer to another PID.
    load_script("cb500x-live.txt", &s);
    char key[32];
    make_key(r.io.last, key, sizeof(key));
    const char *wrong = strcmp(key, "010C0D") == 0 ? "4105730F430E90\r" : "410C2EE00D32\r";
    reply_text(&r, wrong);
    CHECK_STR(r.state.last_error, "stray reply");
    CHECK(r.elm.in_flight);   // still waiting for ours
}

// A test ride: the link went quiet ~4 s, two polls timed out, and the reinit
// took the late STOPPED as ATWS's answer. Every reply then landed one command
// late (0100 got "OK") and the ECU looked off for a minute. The adapter's
// output, in order, as the raw log has it from raw 2101 on:
static void test_desync_after_timeouts(void)
{
    static script_t s;
    rig_t r;
    replay(&r, "cb500x-live.txt", 60);
    CHECK(r.state.phase == PHASE_LIVE);
    load_script("cb500x-live.txt", &s);
    r.t += 1300;
    elm_tick(&r.elm, r.t);   // a lost reply: on with the next poll
    r.t += 1300;
    elm_tick(&r.elm, r.t);   // a second: the session starts again
    CHECK_STR(r.io.last, "ATWS\r");
    const char *stream[] = {"STOPPED\r", "ELM327 v2.3\r", "ATE0\rOK\r", "OK\r", "OK\r", "OK\r", "OK\r",
                            "ELM327 v2.3\r", "OK\r"};
    for (size_t i = 0; i < sizeof(stream) / sizeof(stream[0]); i++) {
        r.t += 60;
        reply_text(&r, stream[i]);
        CHECK(r.state.phase != PHASE_ECU_OFF);
    }
    CHECK_STR(r.io.last, "0100\r");   // the script ran in step
    CHECK_STR(r.state.adapter, "ELM327 v2.3");
    bool off = false;
    for (int i = 0; i < 30; i++) {
        char key[32];
        make_key(r.io.last, key, sizeof(key));
        reply_text(&r, find_reply(&s, key));
        off |= r.state.phase == PHASE_ECU_OFF;
        r.t += 60;
        elm_tick(&r.elm, r.t);
    }
    CHECK(!off);
    CHECK(r.state.phase == PHASE_LIVE);
    CHECK(!r.elm.unsynced);

    // With replies in step, text is taken as it comes: a clone answering ATWS
    // with OK still gets through its init.
    rig_start(&r);
    CHECK_STR(r.io.last, "ATWS\r");
    reply_text(&r, "OK\r");
    CHECK_STR(r.io.last, "ATE0\r");
}

// A test ride: the adapter was powered at key-on and sent its banner
// unasked, at once, before ATWS's own answer. The banner went to ATWS; the
// real "ATWS ELM327 v2.3" must not land on ATE0, or every reply after it is one
// command late and 0100 gets "OK" (the ECU looked off for 5 s).
static void test_banner_after_key_on(void)
{
    rig_t r;
    rig_start(&r);
    CHECK_STR(r.io.last, "ATWS\r");
    const char *stream[] = {"ELM327 v2.3\r", "ATWS\rELM327 v2.3\r", "ATE0\rOK\r", "OK\r", "OK\r", "OK\r",
                            "OK\r", "ELM327 v2.3\r"};
    for (size_t i = 0; i < sizeof(stream) / sizeof(stream[0]); i++) {
        r.t += 60;
        reply_text(&r, stream[i]);
    }
    CHECK_STR(r.io.last, "ATSP0\r");   // no saved protocol in the rig
    reply_text(&r, "OK\r");
    CHECK_STR(r.io.last, "0100\r");
    CHECK_STR(r.state.adapter, "ELM327 v2.3");
    reply_text(&r, "41007C3EC011\r");
    CHECK(r.state.phase != PHASE_ECU_OFF);
    CHECK_STR(r.io.last, "ATDPN\r");
}

static void test_clear_codes(void)
{
    rig_t r;
    replay(&r, "cb500x-live.txt", 60);
    // Engine at idle (RPM 1205, fresh): refused.
    r.t += 10;
    obd_state_set(&r.state, PID_RPM, 1205.0f, r.t);
    CHECK(!elm_request_clear(&r.elm, r.t));
    // Engine stopped: accepted, sent before the next poll.
    obd_state_set(&r.state, PID_RPM, 0.0f, r.t);
    CHECK(elm_request_clear(&r.elm, r.t));
    CHECK(r.state.clear == CLEAR_PENDING || r.state.clear == CLEAR_SENT);
    // Finish the request in flight, then the 04 goes out.
    for (int i = 0; i < 4 && strcmp(r.io.last, "04\r") != 0; i++) {
        reply_text(&r, "NO DATA\r");
    }
    CHECK_STR(r.io.last, "04\r");
    reply_text(&r, "44\r");
    CHECK(r.state.clear == CLEAR_OK);
    CHECK_STR(r.io.last, "03\r");   // read back at once
}

static void test_gear(void)
{
    // The example profile (honda_cb500x.h): six gears, K 48.3.
    CHECK(gear_count() == 6);
    CHECK_NEAR(gear_rpm_per_kmh(4), 1.300f * 48.3f, 0.01);
    CHECK(gear_rpm_per_kmh(0) == 0 && gear_rpm_per_kmh(7) == 0);
    for (int g = 1; g <= gear_count(); g++) {
        for (float kmh = 20; kmh <= 120; kmh += 25) {
            float rpm = kmh * gear_rpm_per_kmh(g);
            if (rpm < 1000 || rpm > 10000) continue;
            CHECK(gear_match(rpm, kmh) == g);
            CHECK(gear_match(rpm * 1.03f, kmh) == g);   // inside the 4.5 % band
        }
    }
    // The rpm per km/h clusters of a real ride: 2nd to 5th.
    CHECK(gear_match(102.5f * 40, 40) == 2);
    CHECK(gear_match(77.5f * 60, 60) == 3);
    CHECK(gear_match(62.8f * 70, 70) == 4);
    CHECK(gear_match(55.5f * 90, 90) == 5);
    // Between two gears (a shift, the clutch in), too slow, or idling: none.
    CHECK(gear_match(0.5f * (gear_rpm_per_kmh(2) + gear_rpm_per_kmh(3)) * 50, 50) == 0);
    CHECK(gear_match(1200, 5) == 0);
    CHECK(gear_match(800, 10) == 0);

    gear_t g = {0};
    CHECK(gear_update(&g, true, 4000, 63.7f) == 0);   // needs a few samples
    gear_update(&g, true, 4000, 63.7f);
    CHECK(gear_update(&g, true, 4000, 63.7f) == 4);
    CHECK(gear_update(&g, false, 0, 0) == 0);
}

static void test_demo_ride(void)
{
    // A whole loop of the demo at its 50 ms feed: geared to the profile, the
    // gear estimate follows the demo's own gear, the revs stay out of the red
    // zone, and the staged events come on time.
    obd_state_t s;
    demo_start(&s, 0);
    gear_t g = {0};
    int moving = 0, agree = 0, max_kmh = 0;
    float max_rpm = 0;
    bool fresh = true, overheated = false, fault = false, stopped = false;
    for (int64_t now = 0; now < 300000; now += 50) {
        demo_feed(&s, now);
        float rpm = 0, kmh = 0, cool = 0;
        fresh &= obd_value(&s, PID_RPM, now, &rpm) && obd_value(&s, PID_SPEED, now, &kmh);
        obd_value(&s, PID_COOLANT, now, &cool);
        int shown = gear_update(&g, true, rpm, kmh);
        if (kmh >= 15) {
            moving++;
            if (shown == demo_gear()) agree++;
        }
        if (rpm > max_rpm) max_rpm = rpm;
        if (kmh > max_kmh) max_kmh = (int)kmh;
        if (cool >= BIKE_COOLANT_ALERT) overheated = true;
        if (s.mil && s.n_dtcs == 1) fault = true;
        if (now >= 290000 && rpm == 0) stopped = true;
    }
    printf("  demo: %d moving samples, gear agrees on %d, max %d km/h, %.0f rpm\n", moving, agree, max_kmh,
           max_rpm);
    CHECK(fresh);
    CHECK(moving > 3000);
    CHECK(agree * 100 >= moving * 95);
    CHECK(max_kmh >= 90 && max_kmh <= 110);
    CHECK(max_rpm < BIKE_RPM_RED);
    CHECK(overheated && fault && stopped);
}

static void test_alerts(void)
{
    alerts_t a;
    alerts_init(&a);
    int64_t t = 0;
    CHECK(alerts_evaluate(&a, true, 100, true, 14.0f, true, 3000, 0, false, t) == ALERT_NONE);
    CHECK(alerts_evaluate(&a, true, 105, true, 14.0f, true, 3000, 0, false, t += 1000) == ALERT_COOLANT);
    CHECK(alerts_evaluate(&a, true, 106, true, 14.0f, true, 3000, 0, false, t += 1000) == ALERT_NONE);
    // Re-armed only 3 °C under the limit.
    alerts_evaluate(&a, true, 103, true, 14.0f, true, 3000, 0, false, t += 1000);
    CHECK(alerts_evaluate(&a, true, 105, true, 14.0f, true, 3000, 0, false, t += 1000) == ALERT_NONE);
    alerts_evaluate(&a, true, 101, true, 14.0f, true, 3000, 0, false, t += 1000);
    CHECK(alerts_evaluate(&a, true, 105, true, 14.0f, true, 3000, 0, false, t += 1000) == ALERT_COOLANT);
    // Low voltage needs 10 s with the engine running.
    alerts_t b;
    alerts_init(&b);
    t = 0;
    for (int i = 0; i < 10; i++) {
        CHECK(alerts_evaluate(&b, true, 80, true, 11.5f, true, 3000, 0, false, t += 1000) == ALERT_NONE);
    }
    CHECK(alerts_evaluate(&b, true, 80, true, 11.5f, true, 3000, 0, false, t += 1000) == ALERT_VOLT_LOW);
    // Engine off: a low battery is no alert.
    alerts_t c;
    alerts_init(&c);
    for (int i = 0; i < 20; i++) {
        CHECK(alerts_evaluate(&c, true, 30, true, 11.5f, true, 0, 0, false, i * 1000) == ALERT_NONE);
    }
    // A new code alerts once.
    CHECK(alerts_evaluate(&c, true, 30, true, 12.5f, true, 0, 1, false, 30000) == ALERT_DTC);
    CHECK(alerts_evaluate(&c, true, 30, true, 12.5f, true, 0, 1, false, 31000) == ALERT_NONE);
}

static void test_battery_alert(void)
{
    alerts_t a;
    alerts_init(&a);
    int64_t t = 0;
    CHECK(alerts_battery(&a, true, 60, false, t += 1000) == ALERT_NONE);
    CHECK(alerts_battery(&a, true, 21, false, t += 1000) == ALERT_NONE);
    CHECK(alerts_battery(&a, true, 20, false, t += 1000) == ALERT_BOARD_BATT);
    CHECK(alerts_battery(&a, true, 19, false, t += 1000) == ALERT_NONE);
    // A reading wobbling around 20 % doesn't fire again: re-arming needs 25 %.
    CHECK(alerts_battery(&a, true, 21, false, t += 1000) == ALERT_NONE);
    CHECK(alerts_battery(&a, true, 20, false, t += 1000) == ALERT_NONE);
    CHECK(alerts_battery(&a, true, 10, false, t += 1000) == ALERT_BOARD_BATT);
    CHECK(alerts_battery(&a, true, 9, false, t += 1000) == ALERT_NONE);
    CHECK(a.fired == 2);
    // Charging re-arms both levels and never fires.
    CHECK(alerts_battery(&a, true, 9, true, t += 1000) == ALERT_NONE);
    CHECK(alerts_battery(&a, true, 9, false, t += 1000) == ALERT_BOARD_BATT);
    // Booting already low warns once, not once per level.
    alerts_t b;
    alerts_init(&b);
    CHECK(alerts_battery(&b, true, 8, false, 1000) == ALERT_BOARD_BATT);
    CHECK(alerts_battery(&b, true, 8, false, 2000) == ALERT_NONE);
    // No cell, or alerts switched off: silent.
    alerts_t c;
    alerts_init(&c);
    CHECK(alerts_battery(&c, false, 0, false, 1000) == ALERT_NONE);
    c.enabled = false;
    CHECK(alerts_battery(&c, true, 5, false, 2000) == ALERT_NONE);
    c.enabled = true;
    CHECK(alerts_battery(&c, true, 5, false, 3000) == ALERT_BOARD_BATT);
}

// The board overheating: each level once per crossing, critical repeats
// every 5 minutes, re-arming 5 degrees below, the cell before the chip.
static void test_board_temp_alert(void)
{
    alerts_t a;
    alerts_init(&a);
    a.enabled = true;
    int64_t t = 0;
    CHECK(alerts_board_temp(&a, true, 60, true, 35, false, t) == ALERT_NONE);
    CHECK(alerts_board_temp(&a, true, 80, true, 35, false, t += 5000) == ALERT_BOARD_HOT);
    CHECK(a.hot_source == HOT_CHIP && !a.hot_critical && a.hot_c == 80);
    CHECK(alerts_board_temp(&a, true, 84, true, 35, false, t += 5000) == ALERT_NONE);   // once
    CHECK(alerts_board_temp(&a, true, 90, true, 35, false, t += 5000) == ALERT_BOARD_HOT);
    CHECK(a.hot_critical);
    CHECK(alerts_board_temp(&a, true, 91, true, 35, false, t += 60000) == ALERT_NONE);
    CHECK(alerts_board_temp(&a, true, 91, true, 35, false, t += 240000) == ALERT_BOARD_HOT);   // 5 min on
    CHECK(a.hot_critical);
    // Cooling just under the warning doesn't re-arm it; 5 degrees under does.
    CHECK(alerts_board_temp(&a, true, 78, true, 35, false, t += 5000) == ALERT_NONE);
    CHECK(alerts_board_temp(&a, true, 81, true, 35, false, t += 5000) == ALERT_NONE);
    CHECK(alerts_board_temp(&a, true, 74, true, 35, false, t += 5000) == ALERT_NONE);
    CHECK(alerts_board_temp(&a, true, 80, true, 35, false, t += 5000) == ALERT_BOARD_HOT);

    // The cell goes first, and the gauge's flag alone is critical.
    alerts_init(&a);
    a.enabled = true;
    CHECK(alerts_board_temp(&a, true, 85, true, 51, false, 0) == ALERT_BOARD_HOT);
    CHECK(a.hot_source == HOT_CELL && !a.hot_critical && a.hot_c == 51);
    CHECK(alerts_board_temp(&a, true, 85, true, 51, false, 1000) == ALERT_BOARD_HOT);   // now the chip
    CHECK(a.hot_source == HOT_CHIP);
    alerts_init(&a);
    a.enabled = true;
    CHECK(alerts_board_temp(&a, false, 0, false, 0, true, 0) == ALERT_BOARD_HOT);
    CHECK(a.hot_source == HOT_CELL && a.hot_critical && isnan(a.hot_c));
    CHECK(alerts_board_temp(&a, true, 50, false, 0, false, 1000) == ALERT_NONE);   // no cell, no chip warning

    // Alerts off: nothing fires and nothing is used up.
    alerts_init(&a);
    a.enabled = false;
    CHECK(alerts_board_temp(&a, true, 95, true, 60, false, 0) == ALERT_NONE);
    a.enabled = true;
    CHECK(alerts_board_temp(&a, true, 95, true, 35, false, 1000) == ALERT_BOARD_HOT && a.hot_critical);
}

static void test_trip(void)
{
    trip_t t;
    trip_reset(&t);
    for (int i = 0; i < 3600; i++) {   // an hour at 60 km/h, sampled every second
        trip_update(&t, 1000, true, 60.0f, true, 4000.0f, true, 90.0f, true, 14.1f);
    }
    CHECK_NEAR(t.distance_m, 60000.0, 1.0);
    CHECK(t.moving_s == 3600 && t.engine_s == 3600);
    CHECK_NEAR(trip_avg_kmh(&t), 60.0, 0.1);
    CHECK_NEAR(t.min_volts, 14.1, 0.01);
    trip_update(&t, 5000, true, 200.0f, true, 9000.0f, true, 99.0f, true, 10.0f);   // a gap: ignored
    CHECK_NEAR(t.max_kmh, 60.0, 0.01);
    // Cranking (10.6 V at 1,678 rpm) doesn't count as the minimum.
    trip_reset(&t);
    for (int i = 0; i < 10; i++) trip_update(&t, 200, true, 0, true, 1678, false, 0, true, 10.6f);
    for (int i = 0; i < 100; i++) trip_update(&t, 200, true, 0, true, 1200, false, 0, true, 14.0f);
    CHECK_NEAR(t.min_volts, 14.0, 0.01);
    // Lean and G held for a second, through the trip.
    for (int i = 0; i < 100; i++) trip_update_motion(&t, 20, i < 60 ? -33.0f : 70.0f * (i % 2 ? 1 : -1), 0.5f);
    CHECK_NEAR(t.max_lean_left, 33.0, 0.01);
    CHECK(t.max_lean_right == 0);
    CHECK_NEAR(t.max_g, 0.5, 0.01);
}

// Rodrigues: v rotated by deg about the unit axis k.
static void rotate(float v[3], const float k[3], float deg)
{
    float t = deg * (float)M_PI / 180, c = cosf(t), sn = sinf(t);
    float kv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
    float x[3] = {k[1] * v[2] - k[2] * v[1], k[2] * v[0] - k[0] * v[2], k[0] * v[1] - k[1] * v[0]};
    for (int i = 0; i < 3; i++) v[i] = v[i] * c + x[i] * sn + k[i] * kv * (1 - c);
}

// One IMU sample of a bike in a steady turn at lean phi (deg, + right), for
// a board whose bike axes sit at up/right (unit vectors in sensor axes).
static void turn_sample(const float up[3], const float right[3], float phi_deg, float kmh, float acc[3], float gyro[3])
{
    const float g = 9.80665f;
    float phi = phi_deg * (float)M_PI / 180;
    float yaw = -tanf(phi) * g / (kmh / 3.6f);   // rad/s, clockwise from above for a right turn
    for (int i = 0; i < 3; i++) {
        // True up in the bike's frame: cos(phi) up - sin(phi) right. The
        // accelerometer sees only the bike's own vertical.
        gyro[i] = yaw * (cosf(phi) * up[i] - sinf(phi) * right[i]) * 180 / (float)M_PI;
        acc[i] = g / cosf(phi) * up[i];
    }
}

// The board looking after itself: heat turns battery saver on (and off once
// cool), too hot or an empty battery switches it off, and it wakes only
// when that is over.
static void test_board_guard(void)
{
    board_guard_t g;
    guard_init(&g);
    guard_in_t in = {.has_chip = true, .chip_c = 60, .has_cell = true, .cell_c = 40, .battery = true, .mv = 3900};
    int64_t t = 0;
    CHECK(guard_update(&g, &in, t) == GUARD_NONE);

    // In the sun: the cell at 50 C. Saver on, once; cooled, off again.
    in.cell_c = 50;
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_SAVER_ON);
    in.saver = true;
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_NONE);
    in.cell_c = 47;   // under the warning, not yet 5 below it
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_NONE);
    in.cell_c = 45;
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_SAVER_OFF);
    in.saver = false;
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_NONE);

    // Switched off by hand while still warm: it stays off this warm spell.
    in.chip_c = 82;
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_SAVER_ON);
    in.saver = true;
    guard_saver_changed(&g);
    in.saver = false;
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_NONE);
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_NONE);
    // ...and a saver the user turned on isn't turned off when it cools.
    in.saver = true;
    in.chip_c = 60;
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_NONE);
    // Already on when it gets warm (the battery alert's): not the heat's to undo.
    in.cell_c = 51;
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_NONE);
    in.cell_c = 40;
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_NONE);
    // Heat's saver with the battery low by the time it cools: left on.
    in.saver = false;
    in.cell_c = 52;
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_SAVER_ON);
    in.saver = true;
    in.low = true;
    in.cell_c = 40;
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_NONE);
    in.low = false;

    // Too hot: a 5 s spike is not enough, 10 s held is.
    in.chip_c = 91;
    CHECK(guard_update(&g, &in, t += 5000) != GUARD_OFF_HOT);
    in.chip_c = 70;
    CHECK(guard_update(&g, &in, t += 5000) != GUARD_OFF_HOT);
    in.cell_c = 58;
    CHECK(guard_update(&g, &in, t += 5000) != GUARD_OFF_HOT);
    CHECK(guard_update(&g, &in, t += 5000) != GUARD_OFF_HOT);
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_OFF_HOT);
    // Waking: still too warm sleeps on; cool starts.
    guard_in_t w = {.has_chip = true, .chip_c = 40, .has_cell = true, .cell_c = 48, .battery = true, .mv = 3900};
    CHECK(!guard_may_wake(GUARD_OFF_HOT, &w));
    w.cell_c = 44;
    CHECK(guard_may_wake(GUARD_OFF_HOT, &w));

    // A long ride's end: the cell under 3.4 V. 30 s held, and never while charging.
    guard_init(&g);
    in = (guard_in_t){.has_chip = true, .chip_c = 55, .has_cell = true, .cell_c = 45, .battery = true, .mv = 3390};
    t = 0;
    for (int i = 0; i < 6; i++) CHECK(guard_update(&g, &in, t += 5000) != GUARD_OFF_EMPTY);
    in.mv = 3420;   // a recovery resets the wait
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_NONE);
    in.mv = 3300;
    for (int i = 0; i < 6; i++) CHECK(guard_update(&g, &in, t += 5000) != GUARD_OFF_EMPTY);
    CHECK(guard_update(&g, &in, t += 5000) == GUARD_OFF_EMPTY);
    in.charging = true;
    guard_init(&g);
    for (int i = 0; i < 10; i++) CHECK(guard_update(&g, &in, t += 5000) == GUARD_NONE);
    in.battery = false;   // no cell (USB only): never empty
    in.charging = false;
    for (int i = 0; i < 10; i++) CHECK(guard_update(&g, &in, t += 5000) == GUARD_NONE);
    // Waking: a rested cell at 3.5 V sleeps on; charging or 3.7 V starts.
    w = (guard_in_t){.battery = true, .mv = 3500};
    CHECK(!guard_may_wake(GUARD_OFF_EMPTY, &w));
    w.charging = true;
    CHECK(guard_may_wake(GUARD_OFF_EMPTY, &w));
    w.charging = false, w.mv = 3700;
    CHECK(guard_may_wake(GUARD_OFF_EMPTY, &w));
    CHECK(guard_may_wake(GUARD_NONE, &w));
}

// Two trips updated on the same tick (the trip and the ride, as the sensor
// task does) each count every second once.
static void test_two_trips(void)
{
    trip_t a, b;
    trip_reset(&a);
    trip_reset(&b);
    for (int i = 0; i < 600; i++) {   // 60 s in 100 ms ticks, some 120 ms
        int dt = i % 3 ? 100 : 120;
        trip_update(&a, dt, true, 50, true, 4000, false, 0, false, 0);
        trip_update(&b, dt, true, 50, true, 4000, false, 0, false, 0);
    }
    CHECK(a.engine_s == 64 && b.engine_s == 64);   // 64 s of ticks
    CHECK(a.moving_s == 64 && b.moving_s == 64);
    CHECK_NEAR(trip_avg_kmh(&a), 50, 0.5);
    CHECK_NEAR(trip_avg_kmh(&b), 50, 0.5);
}

static void test_motion(void)
{
    // Board upright, screen facing the rider, IMU z out of the screen (flip -1):
    // bike up = +y, right = +x, forward = -z.
    motion_est_t m;
    motion_init(&m, -1.0f);
    const float g = 9.80665f;
    float upright[3] = {0, g, 0};
    float up[3] = {0, 1, 0}, right[3] = {1, 0, 0};
    float acc[3], gyro[3];
    motion_zero(&m, upright);

    // Standing still and tilted 20 degrees (side stand, a loose case): no lean.
    float s20 = sinf(20 * (float)M_PI / 180), c20 = cosf(20 * (float)M_PI / 180);
    float tilted[3] = {-s20 * g, c20 * g, 0};
    float still[3] = {0, 0, 0};
    for (int i = 0; i < 100; i++) motion_update(&m, tilted, still, 0.02f, 0);
    CHECK(!m.valid);
    CHECK_NEAR(m.lean, 0.0, 0.01);
    CHECK_NEAR(m.lon_g, 0.0, 0.01);

    // Speed hysteresis: on at 15 km/h, off below 10, unknown speed is off.
    motion_update(&m, upright, still, 0.02f, 12.0f);
    CHECK(!m.valid);
    motion_update(&m, upright, still, 0.02f, 15.0f);
    CHECK(m.valid);
    motion_update(&m, upright, still, 0.02f, 11.0f);
    CHECK(m.valid);
    motion_update(&m, upright, still, 0.02f, 9.0f);
    CHECK(!m.valid);
    motion_update(&m, upright, still, 0.02f, 20.0f);
    motion_update(&m, upright, still, 0.02f, -1.0f);
    CHECK(!m.valid);

    // Riding at 36 km/h in a steady right turn at 30 degrees of lean.
    motion_zero(&m, upright);
    for (int i = 0; i < 300; i++) {
        turn_sample(up, right, 30, 36, acc, gyro);
        motion_update(&m, acc, gyro, 0.02f, 36.0f);
    }
    CHECK(m.valid);
    CHECK_NEAR(m.lean, 30.0, 1.5);
    CHECK_NEAR(m.lat_g, tanf(30 * (float)M_PI / 180), 0.05);

    // A loose case: the board sits pitched 30 and rolled 20 degrees off the
    // bike, but the zero was taken upright on the desk. Riding straight
    // teaches it the bike's up; then a 26 degree left turn reads right.
    float mup[3] = {0, 1, 0}, mright[3] = {1, 0, 0}, mfwd[3] = {0, 0, -1};
    rotate(mup, right, 30); rotate(mfwd, right, 30);
    rotate(mup, mfwd, 20); rotate(mright, mfwd, 20);
    motion_init(&m, -1.0f);
    motion_zero(&m, upright);
    for (int i = 0; i < 500; i++) {   // 10 s straight at 50 km/h
        turn_sample(mup, mright, 0, 50, acc, gyro);
        motion_update(&m, acc, gyro, 0.02f, 50.0f);
    }
    CHECK(m.calibrated);
    CHECK_NEAR(m.up[0] * mup[0] + m.up[1] * mup[1] + m.up[2] * mup[2], 1.0, 0.001);
    CHECK_NEAR(m.lean, 0.0, 1.0);
    for (int i = 0; i < 300; i++) {
        turn_sample(mup, mright, -26, 50, acc, gyro);
        motion_update(&m, acc, gyro, 0.02f, 50.0f);
    }
    CHECK_NEAR(m.lean, -26.0, 2.0);

    // Slowing to a stop: lean and G drop out instead of freezing.
    for (int i = 0; i < 10; i++) motion_update(&m, acc, gyro, 0.02f, 5.0f);
    CHECK(!m.valid);
    CHECK_NEAR(m.lean, 0.0, 0.01);

    // In a tank bag: a 0.2 s jolt spinning the board at 400 deg/s about
    // its forward axis, mid-corner, barely moves the lean.
    float fwd[3] = {0, 0, -1};
    motion_init(&m, -1.0f);
    motion_zero(&m, upright);
    for (int i = 0; i < 300; i++) {
        turn_sample(up, right, 24, 40, acc, gyro);
        motion_update(&m, acc, gyro, 0.02f, 40.0f);
    }
    CHECK_NEAR(m.lean, 24.0, 1.5);
    float worst = 0;
    for (int i = 0; i < 10; i++) {
        turn_sample(up, right, 24, 40, acc, gyro);
        for (int k = 0; k < 3; k++) gyro[k] += 400 * fwd[k];
        motion_update(&m, acc, gyro, 0.02f, 40.0f);
        if (fabsf(m.lean - 24) > worst) worst = fabsf(m.lean - 24);
    }
    for (int i = 0; i < 25; i++) {   // the half second after
        turn_sample(up, right, 24, 40, acc, gyro);
        motion_update(&m, acc, gyro, 0.02f, 40.0f);
        if (fabsf(m.lean - 24) > worst) worst = fabsf(m.lean - 24);
    }
    CHECK(worst < 5.0f);
    CHECK(m.shaken_s > 0);
    for (int i = 0; i < 200; i++) {
        turn_sample(up, right, 24, 40, acc, gyro);
        motion_update(&m, acc, gyro, 0.02f, 40.0f);
    }
    CHECK(m.shaken_s <= 0);
    CHECK_NEAR(m.lean, 24.0, 3.0);

    // A turn rate under the jolt limit that no bike reaches at 60 km/h
    // (150 deg/s would be 4.4 g): lateral g stops at 1.2.
    for (int i = 0; i < 50; i++) {
        float spin[3] = {0, -150, 0};
        motion_update(&m, upright, spin, 0.02f, 60.0f);
    }
    CHECK(m.lat_g <= 1.2f + 1e-4f);

    // A gyro offset read as a steady turn, ~10 degrees left on every
    // straight. Riding straight before any stop, an offset leans the bike...
    const float bias[3] = {1.5f, -2.5f, 3.0f};
    motion_init(&m, -1.0f);
    motion_zero(&m, upright);
    for (int i = 0; i < 500; i++) motion_update(&m, upright, bias, 0.02f, 80.0f);
    CHECK(fabsf(m.lean) > 2.0f);
    // ...a 5 s stop at idle (the engine's shake on top) teaches the offset...
    for (int i = 0; i < 250; i++) {
        float shake[3];
        for (int k = 0; k < 3; k++) shake[k] = bias[k] + (i % 2 ? 10.0f : -10.0f);
        motion_update(&m, upright, shake, 0.02f, 0.0f);
    }
    for (int k = 0; k < 3; k++) CHECK_NEAR(m.gyro_bias[k], bias[k], 0.15);
    // ...and after it a straight reads upright, and a turn reads its lean.
    for (int i = 0; i < 500; i++) motion_update(&m, upright, bias, 0.02f, 80.0f);
    CHECK_NEAR(m.lean, 0.0, 1.0);
    CHECK_NEAR(m.lat_g, 0.0, 0.02);
    for (int i = 0; i < 300; i++) {
        turn_sample(up, right, 30, 60, acc, gyro);
        for (int k = 0; k < 3; k++) gyro[k] += bias[k];
        motion_update(&m, acc, gyro, 0.02f, 60.0f);
    }
    CHECK_NEAR(m.lean, 30.0, 1.5);
    // The board picked up at a stop (fast turns, or off 1 g) teaches nothing.
    float learned = m.gyro_bias[1];
    for (int i = 0; i < 100; i++) {
        float handled[3] = {0, 80.0f, 0};
        motion_update(&m, upright, handled, 0.02f, 0.0f);
    }
    float lifted[3] = {0, 1.5f * g, 0};
    for (int i = 0; i < 100; i++) motion_update(&m, lifted, still, 0.02f, 0.0f);
    CHECK_NEAR(m.gyro_bias[1], learned, 1e-4);
}

// The largest value held for a full second, each side.
static void test_held_peak(void)
{
    held_peak_t h;
    held_reset(&h);
    for (int i = 0; i < 100; i++) held_add(&h, 20, 30.0f + (i >= 40 && i < 50 ? 40.0f : 0));   // 2 s, a 0.2 s spike
    CHECK_NEAR(h.peak_pos, 30.0, 0.01);
    CHECK(h.peak_neg == 0);
    // A tank-bag tumble from a real log: +41, -24, -70, -55, -35 within a second and a half.
    const float tumble[] = {41, -24, -70, -55, -35, -54, -37, -29, -28, -9};
    held_reset(&h);
    for (int i = 0; i < 10; i++) {
        for (int k = 0; k < 10; k++) held_add(&h, 20, tumble[i]);   // each value 0.2 s
    }
    CHECK_NEAR(h.peak_neg, 35.0, 0.01);   // -70 -55 -35 -54 -37 is the second held: not 70
    CHECK(h.peak_pos == 0);
    // A gap (NAN: stopped) breaks a second in two.
    held_reset(&h);
    for (int i = 0; i < 30; i++) held_add(&h, 20, -20);
    held_add(&h, 20, NAN);
    for (int i = 0; i < 30; i++) held_add(&h, 20, -20);
    CHECK(h.peak_neg == 0);
    for (int i = 0; i < 30; i++) held_add(&h, 20, -20);
    CHECK_NEAR(h.peak_neg, 20.0, 0.01);
}

static void test_fuel(void)
{
    // Warm idle: 1,200 rpm, 22 % load, closed loop. The CB500X burns roughly
    // 0.3-0.4 L/h idling.
    fuel_in_t idle = {true, true, true, 1200, 22, 0, 2};
    float lph = fuel_rate_lph(&idle);
    CHECK(lph > 0.25f && lph < 0.5f);
    // Cruise near 100 km/h in 6th: ~5,000 rpm, ~50 % load -> 3-5 L/h (~4 L/100 km).
    fuel_in_t cruise = {true, true, true, 5000, 50, 0, 2};
    float c = fuel_rate_lph(&cruise);
    CHECK(c > 3.0f && c < 5.0f);
    // Short-term trim +10 % adds 10 % in closed loop.
    cruise.stft_pct = 10;
    CHECK_NEAR(fuel_rate_lph(&cruise), c * 1.10f, 0.01);
    // Status 4 at a closed-throttle idle (1,200 rpm, 27 % in a real log) is fuelled...
    fuel_in_t idle4 = {true, true, false, 1200, 27, 0, 4};
    CHECK(fuel_rate_lph(&idle4) > 0.25f);
    // ...but low load well above idle is a decel fuel cut.
    fuel_in_t cut = {true, true, false, 2750, 12, 0, 4};
    CHECK_NEAR(fuel_rate_lph(&cut), 0.0, 1e-6);
    // Status 4 at high load: power enrichment, richer than stoichiometric.
    fuel_in_t wot = {true, true, false, 6000, 95, 0, 4}, wot2 = wot;
    wot2.status = 0;
    CHECK(fuel_rate_lph(&wot) > fuel_rate_lph(&wot2));
    // Engine off is zero; no rpm or no load is unknown.
    fuel_in_t off = {true, true, false, 0, 0, 0, 0};
    CHECK_NEAR(fuel_rate_lph(&off), 0.0, 1e-6);
    fuel_in_t noload = {true, false, false, 3000, 0, 0, 2};
    CHECK(fuel_rate_lph(&noload) < 0);

    // Integration: an hour at 4 L/h over 100 km is 4 L, 4 L/100 km.
    trip_t t;
    trip_reset(&t);
    for (int i = 0; i < 3600; i++) {
        trip_update(&t, 1000, true, 100.0f, true, 5000.0f, false, 0, false, 0);
        trip_add_fuel(&t, 1000, true, 4.0f);
    }
    CHECK_NEAR(t.fuel_raw_ml, 4000.0, 0.5);
    CHECK_NEAR(trip_raw_l100(&t), 4.0, 0.01);
    trip_add_fuel(&t, 1000, true, -1.0f);   // running, no estimate
    CHECK(t.fuel_missing_ms == 1000 && fuel_coverage_pct(t.fuel_covered_ms, t.fuel_missing_ms) == 99);

    fill_t f;
    fill_reset(&f);
    CHECK(f.fills == 0 && f.k == 1.0f);
    for (int i = 0; i < 1800; i++) fill_add(&f, 1000, true, 4.0f, true, 100.0f);   // half an hour
    CHECK_NEAR(f.km, 50.0, 0.01);
    CHECK_NEAR(f.raw_ml, 2000.0, 0.5);
    fill_mark(&f);
    CHECK(f.fills == 1 && f.km == 0 && f.raw_ml == 0 && f.k == 1.0f);
}

static void read_fixture(const char *name, char *out, size_t cap)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/%s", FIXTURE_DIR, name);
    FILE *f = fopen(path, "r");
    if (!f) {
        printf("  cannot open %s\n", path);
        exit(2);
    }
    size_t n = fread(out, 1, cap - 1, f);
    out[n] = 0;
    fclose(f);
}

static void test_ride_json(void)
{
    char text[2048];
    ride_summary_t r;
    // Format 2, as the README documents it.
    read_fixture("ride-format2.json", text, sizeof(text));
    CHECK(ride_summary_parse(text, &r));
    CHECK(!r.in_progress);
    CHECK(r.duration_s == 4325 && r.engine_s == 3880);
    CHECK_NEAR(r.km, 63.48, 1e-4);
    CHECK_NEAR(r.avg_kmh, 58, 1e-4);
    CHECK_NEAR(r.max_kmh, 131, 1e-4);
    CHECK_NEAR(r.max_rpm, 8712, 1e-4);
    CHECK_NEAR(r.max_coolant, 98, 1e-4);
    CHECK_NEAR(r.min_volts, 13.62, 1e-4);
    CHECK_NEAR(r.lean_left, 34, 1e-4);
    CHECK_NEAR(r.lean_right, 29, 1e-4);
    CHECK_NEAR(r.max_g, 0.71, 1e-4);
    CHECK(r.alerts == 2);
    CHECK_STR(r.codes, "P0118 P0562");
    CHECK_NEAR(r.fuel_raw_l, 2.61, 1e-4);
    CHECK_NEAR(r.l100_raw, 4.11, 1e-4);
    CHECK_NEAR(r.k, 1.08, 1e-4);
    CHECK(r.batt_start == 82 && r.batt_end == 74);
    uint32_t all = RS_DURATION | RS_ENGINE | RS_DISTANCE | RS_AVG | RS_MAX_KMH | RS_MAX_RPM | RS_MAX_COOLANT
                   | RS_MIN_VOLTS | RS_LEAN | RS_MAX_G | RS_ALERTS | RS_CODES | RS_FUEL | RS_L100 | RS_BATTERY;
    CHECK(r.have == all);

    // Cut short by a power cut: refused, so the reader tries ride.json.tmp.
    char cut[400];
    snprintf(cut, sizeof(cut), "%.*s", 300, text);
    CHECK(!ride_summary_parse(cut, &r));
    // No format key, or an older one: not ours.
    CHECK(!ride_summary_parse("", &r));
    CHECK(!ride_summary_parse("t_s,rpm,kmh\n1,2,3\n", &r));
    CHECK(!ride_summary_parse("{\"duration_s\": 5}", &r));
    CHECK(!ride_summary_parse("{\"format\": 1, \"duration_s\": 5}", &r));
    CHECK(!ride_summary_parse("{\"format\": 2, \"duration_s\" 5}", &r));

    // A fresh report: every value unknown comes out as null and is left unset.
    ride_report_t rep;
    ride_report_init(&rep);
    rep.ride = 3;
    rep.in_progress = true;
    snprintf(rep.uid, sizeof(rep.uid), "00c0ffee");
    size_t n = ride_report_json(&rep, text, sizeof(text));
    CHECK(n > 0 && n == strlen(text));
    CHECK(strstr(text, "\"start\": null") && strstr(text, "\"max_rpm\": null"));
    CHECK(strstr(text, "\"fault_codes\": []") && strstr(text, "\"batt_end_pct\": null"));
    CHECK(strstr(text, "\"max_chip_c\": null") && strstr(text, "\"max_cell_c\": null"));
    CHECK(strstr(text, "\"avg_ma\": null, \"full_mah\": null, \"remaining_mah\": null}") != NULL);
    CHECK(strstr(text, "\"gyro_bias_dps\": null") != NULL);
    CHECK(ride_summary_parse(text, &r));
    CHECK(r.in_progress && r.have == (RS_DURATION | RS_ENGINE | RS_ALERTS | RS_CODES));
    CHECK(r.batt_start == -1 && r.batt_end == -1 && r.codes[0] == 0);

    // A full report: what's written reads back, and a quote in a name is escaped.
    rep.in_progress = false;
    snprintf(rep.start, sizeof(rep.start), "2026-10-17T09:12:03Z");
    snprintf(rep.bike, sizeof(rep.bike), "%s", BIKE_NAME);
    rep.duration_s = 4325, rep.engine_s = 3880;
    rep.distance_km = 63.481f, rep.avg_kmh = 58, rep.max_kmh = 131, rep.max_rpm = 8712;
    rep.max_coolant_c = 98, rep.min_volts = 13.62f, rep.max_lean_left_deg = 34, rep.max_lean_right_deg = 29;
    rep.max_g = 0.71f, rep.alerts = 2;
    snprintf(rep.codes, sizeof(rep.codes), "P0118 P0562");
    rep.fuel_raw_l = 2.61f, rep.fuel_raw_l100 = 4.11f, rep.fuel_k = 1.08f;
    rep.batt_start_pct = 82, rep.batt_end_pct = 74, rep.batt_end_mv = 3850, rep.batt_cell = true;
    rep.batt_avg_ma = -248.4f;
    rep.batt_full_mah = 500, rep.batt_remaining_mah = 212;
    rep.gyro_bias_dps[0] = 1.234f, rep.gyro_bias_dps[1] = -2.5f, rep.gyro_bias_dps[2] = 0;
    rep.tyre[0] = (ride_tyre_t){228, 241, 31, true, 57, -94};
    snprintf(rep.ble_name, sizeof(rep.ble_name), "Bob's \"OBD\"\\1");
    snprintf(rep.vin, sizeof(rep.vin), "TESTVIN0000000000");
    snprintf(rep.cal_id[0], sizeof(rep.cal_id[0]), "37805-MKP-D940");
    n = ride_report_json(&rep, text, sizeof(text));
    CHECK(n > 0);
    CHECK(strstr(text, "\"name\": \"Bob's \\\"OBD\\\"\\\\1\"") != NULL);
    CHECK(strstr(text, "\"start\": \"2026-10-17T09:12:03Z\"") != NULL);
    CHECK(strstr(text, "\"bike\": \"CB500X\"") != NULL);
    CHECK(strstr(text, "\"cal_ids\": [\"37805-MKP-D940\"], \"cvns\": []") != NULL);
    CHECK(strstr(text, "\"front\": {\"paired\": true, \"min_kpa\": 228") != NULL);
    CHECK(strstr(text, "\"rear\": {\"paired\": false, \"min_kpa\": null") != NULL);
    CHECK(strstr(text, "\"max_c\": 31, \"heard\": 57, \"min_rssi_dbm\": -94}") != NULL);
    CHECK(strstr(text, "\"heard\": 0, \"min_rssi_dbm\": null}") != NULL);
    CHECK(strstr(text, "\"charging\": false, \"avg_ma\": -248, \"full_mah\": 500, \"remaining_mah\": 212}") != NULL);
    CHECK(strstr(text, "\"gyro_bias_dps\": [1.23, -2.50, 0.00]") != NULL);
    CHECK(ride_summary_parse(text, &r));
    CHECK(r.have == all);
    CHECK(r.duration_s == 4325 && r.alerts == 2 && r.batt_start == 82 && r.batt_end == 74);
    CHECK_NEAR(r.km, 63.48, 1e-4);
    CHECK_NEAR(r.min_volts, 13.62, 1e-4);
    CHECK_STR(r.codes, "P0118 P0562");
    // Too small a buffer: nothing half-written.
    CHECK(ride_report_json(&rep, text, 200) == 0);
}

// The clock's registers: a known date, a round trip across the century, a
// stopped oscillator and a corrupt register.
static void test_rtc_regs(void)
{
    uint8_t r[RTC_REGS_N];
    int64_t t;
    rtc_regs_encode(1792228323, r);   // 2026-10-17 09:12:03 UTC, a Saturday
    CHECK(r[0] == 0x03 && r[1] == 0x12 && r[2] == 0x09);
    CHECK(r[3] == 0x17 && r[4] == 6 && r[5] == 0x10 && r[6] == 0x26);
    CHECK(rtc_regs_decode(r, &t) && t == 1792228323);
    const int64_t samples[] = {946684800, 951782400, 1767225599, 1767225600, 4102444799};
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        rtc_regs_encode(samples[i], r);
        CHECK(rtc_regs_decode(r, &t) && t == samples[i]);
    }
    rtc_regs_encode(1792228323, r);
    r[0] |= 0x80;                       // the oscillator stopped: power was lost
    CHECK(!rtc_regs_decode(r, &t));
    rtc_regs_encode(1792228323, r);
    r[5] = 0x13;                        // month 13
    CHECK(!rtc_regs_decode(r, &t));
    r[5] = 0x1A;                        // not BCD
    CHECK(!rtc_regs_decode(r, &t));
}

int main(void)
{
    struct { const char *name; void (*fn)(void); } tests[] = {
        {"parse", test_parse},
        {"assembler", test_assembler},
        {"pids", test_pids},
        {"fuel_status_decode", test_fuel_status_decode},
        {"replay_live", test_replay_live},
        {"mode09", test_mode09},
        {"replay_fault", test_replay_fault},
        {"replay_single_pid", test_replay_single_pid},
        {"replay_ignition_off", test_replay_ignition_off},
        {"stray_reply", test_stray_reply},
        {"desync_after_timeouts", test_desync_after_timeouts},
        {"banner_after_key_on", test_banner_after_key_on},
        {"clear_codes", test_clear_codes},
        {"gear", test_gear},
        {"demo ride", test_demo_ride},
        {"alerts", test_alerts},
        {"battery alert", test_battery_alert},
        {"board temperature alert", test_board_temp_alert},
        {"board guard", test_board_guard},
        {"trip", test_trip},
        {"two trips", test_two_trips},
        {"fuel", test_fuel},
        {"motion", test_motion},
        {"held_peak", test_held_peak},
        {"ride json", test_ride_json},
        {"rtc registers", test_rtc_regs},
        {"tpms adv", test_tpms_adv},
        {"tpms formats", test_tpms_formats},
        {"tpms tyres", test_tpms_tyres},
        {"tpms alerts", test_tpms_alerts},
        {"tpms pair", test_tpms_pair},
        {"tpms config file", test_tpms_cfgfile},
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = s_failures;
        printf("%s\n", tests[i].name);
        tests[i].fn();
        printf("  %s\n", s_failures == before ? "ok" : "FAILED");
    }
    printf("%d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
