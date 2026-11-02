#include <math.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "app.h"
#include "bike/bike.h"
#include "ble/ble_scan.h"
#include "sd_log.h"
#include "tpms/tpms_cfgfile.h"
#include "tpms_service.h"

static const char *TAG = "tpms";

const char *const TPMS_TYRE_NAMES[TPMS_TYRES] = {"front", "rear"};
const char *const TPMS_TYRE_CAPS[TPMS_TYRES] = {"FRONT", "REAR"};

#define PKT_QUEUE   16
#define CMD_QUEUE   4
#define LOG_MS      60000        // a paired sensor's line in the logs, at most once a minute
#define SEEN_LOG_MS 300000       // an unpaired sensor's line, once every 5 minutes
#define SEEN_MAX    8
#define CARD_MS     1000         // how often to look for a freshly read tpms.cfg

// One advertisement, as the scan callback hands it over. Legacy advertising
// is at most 31 bytes.
typedef struct {
    uint8_t addr[6];
    int8_t rssi;
    uint8_t len;
    uint8_t data[31];
} pkt_t;

typedef enum { C_PAIR_START, C_PAIR_STOP, C_PAIR_PICK, C_FORGET, C_SET_CFG } cmd_type_t;

typedef struct {
    cmd_type_t type;
    int tyre;
    tpms_pairing_t sensor;
    tpms_cfg_t cfg;
} cmd_t;

static QueueHandle_t s_pkts, s_cmds;
static volatile bool s_want_scan;

// The sensor task's own state.
static EXT_RAM_BSS_ATTR struct {
    tpms_pairing_t pair[TPMS_TYRES];
    tpms_cfg_t cfg;
    tpms_tyre_t tyre[TPMS_TYRES];
    int rssi[TPMS_TYRES];
    int64_t logged_at[TPMS_TYRES];
    int pairing;
    int64_t pair_until;
    tpms_cands_t cands;
    struct { tpms_pairing_t sensor; int64_t logged_at; } seen[SEEN_MAX];
    uint32_t card_seq;
    int64_t card_at;
    char card_text[SD_TPMS_CFG_MAX];
    bool demo;
    tpms_alerts_t alerts;
    float ride_min_kpa[TPMS_TYRES], ride_max_kpa[TPMS_TYRES], ride_max_c[TPMS_TYRES];
    int ride_heard[TPMS_TYRES], ride_min_rssi[TPMS_TYRES];
    uint32_t reading_seq[TPMS_TYRES];
    float reading_kpa[TPMS_TYRES], reading_c[TPMS_TYRES];
    int reading_rssi[TPMS_TYRES];
    int64_t reading_at[TPMS_TYRES];
} s;

static void ride_reset(void)
{
    for (int t = 0; t < TPMS_TYRES; t++) {
        s.ride_min_kpa[t] = s.ride_max_kpa[t] = s.ride_max_c[t] = NAN;
        s.ride_heard[t] = s.ride_min_rssi[t] = 0;
    }
}

// A paired tyre's packet: counted, and its reading kept for events.csv when it
// changed or a minute went by. A sensor can be heard in only a few short spells;
// these lines show how often each sensor really gets through.
static void note_packet(int t, const tpms_reading_t *r, int rssi, int64_t now)
{
    s.ride_heard[t]++;
    if (s.ride_min_rssi[t] == 0 || rssi < s.ride_min_rssi[t]) s.ride_min_rssi[t] = rssi;
    if (!r->awake || isnan(r->kpa)) return;
    bool same = s.reading_seq[t] && r->kpa == s.reading_kpa[t]
                && (r->temp_c == s.reading_c[t] || (isnan(r->temp_c) && isnan(s.reading_c[t])));
    if (same && now - s.reading_at[t] < LOG_MS) return;
    s.reading_seq[t]++;
    s.reading_kpa[t] = r->kpa;
    s.reading_c[t] = r->temp_c;
    s.reading_rssi[t] = rssi;
    s.reading_at[t] = now;
}

// After each update of a tyre: this ride's extremes.
static void track(int t)
{
    const tpms_tyre_t *y = &s.tyre[t];
    if (!isnan(y->kpa)) {
        if (isnan(s.ride_min_kpa[t]) || y->kpa < s.ride_min_kpa[t]) s.ride_min_kpa[t] = y->kpa;
        if (isnan(s.ride_max_kpa[t]) || y->kpa > s.ride_max_kpa[t]) s.ride_max_kpa[t] = y->kpa;
    }
    if (!isnan(y->temp_c) && (isnan(s.ride_max_c[t]) || y->temp_c > s.ride_max_c[t])) s.ride_max_c[t] = y->temp_c;
}

// ---- The scan callback (NimBLE host task) ------------------------------------

static void on_adv(const struct ble_gap_disc_desc *d)
{
    if (!s_pkts || d->length_data == 0 || d->length_data > sizeof(((pkt_t *)0)->data)) {
        return;
    }
    pkt_t p = {.rssi = d->rssi, .len = d->length_data};
    memcpy(p.addr, d->addr.val, 6);
    memcpy(p.data, d->data, d->length_data);
    xQueueSend(s_pkts, &p, 0);   // full: the next one will do
}

// ---- Saving: NVS and the card ---------------------------------------------------

static void to_card(void)
{
    static EXT_RAM_BSS_ATTR char text[SD_TPMS_CFG_MAX];
    tpms_cfgfile_write(text, sizeof(text), s.pair, &s.cfg);
    sd_log_tpms_cfg_put(text);
}

static void save(void)
{
    nvs_set_blob_("tpms_pair", s.pair, sizeof(s.pair));
    nvs_set_blob_("tpms_cfg", &s.cfg, sizeof(s.cfg));
    to_card();
}

static void tail_of(const tpms_pairing_t *p, char out[7])
{
    tpms_pairing_tail(p, out);
}

static bool paired(int t)
{
    return s.pair[t].format != TPMS_F_NONE;
}

static void update_want_scan(void)
{
    bool any = s.pairing >= 0;
    for (int t = 0; t < TPMS_TYRES; t++) any = any || paired(t);
    s_want_scan = app_settings()->tpms && !s.demo && any;
}

// A card was mounted (or formatted): a tpms.cfg on it wins; with none, ours
// goes onto it.
static void check_card(int64_t now)
{
    if (now - s.card_at < CARD_MS) return;
    s.card_at = now;
    if (!sd_log_tpms_cfg_get(s.card_text, sizeof(s.card_text), &s.card_seq)) return;
    if (!s.card_text[0]) {
        ESP_LOGI(TAG, "no tpms.cfg on the card: writing ours");
        to_card();
        return;
    }
    tpms_pairing_t pair[TPMS_TYRES];
    tpms_cfg_t cfg;
    if (!tpms_cfgfile_parse(s.card_text, pair, &cfg)) {
        ESP_LOGW(TAG, "tpms.cfg on the card has no settings in it: left alone");
        return;
    }
    if (memcmp(pair, s.pair, sizeof(pair)) == 0 && memcmp(&cfg, &s.cfg, sizeof(cfg)) == 0) {
        return;
    }
    for (int t = 0; t < TPMS_TYRES; t++) {
        if (memcmp(&pair[t], &s.pair[t], sizeof(pair[t])) != 0) {
            tpms_tyre_init(&s.tyre[t]);
            s.rssi[t] = 0;
            s.logged_at[t] = 0;
        }
    }
    memcpy(s.pair, pair, sizeof(pair));
    s.cfg = cfg;
    nvs_set_blob_("tpms_pair", s.pair, sizeof(s.pair));
    nvs_set_blob_("tpms_cfg", &s.cfg, sizeof(s.cfg));
    char f[7], r[7];
    tail_of(&s.pair[0], f);
    tail_of(&s.pair[1], r);
    ESP_LOGI(TAG, "settings from the card's tpms.cfg: front %s %s, rear %s %s",
             TPMS_FORMAT_NAMES[s.pair[0].format], paired(0) ? f : "-", TPMS_FORMAT_NAMES[s.pair[1].format],
             paired(1) ? r : "-");
}

// ---- Packets ----------------------------------------------------------------------

static void hex(const uint8_t *b, int n, char *out)
{
    for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02X", b[i]);
    out[2 * n] = 0;
}

static void log_reading(const char *who, const tpms_reading_t *r, const tpms_pairing_t *id, const pkt_t *p,
                        int64_t now)
{
    char tail[7], raw[2 * 31 + 1], line[200];
    tail_of(id, tail);
    hex(p->data, p->len, raw);
    snprintf(line, sizeof(line), "tpms %s %s %s %.0f kPa %.0f C %.2f V %d dB raw %s", who,
             TPMS_FORMAT_NAMES[r->format], tail, r->kpa, r->temp_c, r->volts, p->rssi, raw);
    ESP_LOGI(TAG, "%s", line + 5);
    sd_log_note(line, now);
}

// An unpaired sensor: logged once every 5 minutes, so a ride's log shows what
// was around without filling up.
static void log_seen(const tpms_reading_t *r, const pkt_t *p, int64_t now)
{
    tpms_pairing_t id;
    tpms_pairing_set(&id, r, p->addr);
    int slot = -1, oldest = 0;
    for (int i = 0; i < SEEN_MAX && slot < 0; i++) {
        if (tpms_pairing_same(&s.seen[i].sensor, &id)) slot = i;
        if (s.seen[i].logged_at < s.seen[oldest].logged_at) oldest = i;
    }
    if (slot >= 0 && now - s.seen[slot].logged_at < SEEN_LOG_MS) return;
    if (slot < 0) slot = oldest;
    s.seen[slot].sensor = id;
    s.seen[slot].logged_at = now;
    log_reading("heard", r, &id, p, now);
}

static void handle(const pkt_t *p, int64_t now)
{
    tpms_adv_t a;
    tpms_reading_t r;
    tpms_adv_parse(&a, p->data, p->len);
    if (!tpms_decode(&a, &r)) return;
    if (s.pairing >= 0) {
        int before = s.cands.n;
        tpms_cands_offer(&s.cands, &r, p->addr, p->rssi, now);
        if (s.cands.n > before) log_reading("pairing found", &r, &s.cands.c[s.cands.n - 1].sensor, p, now);
    }
    bool mine = false;
    for (int t = 0; t < TPMS_TYRES; t++) {
        if (!tpms_pairing_matches(&s.pair[t], &r, p->addr)) continue;
        mine = true;
        tpms_tyre_update(&s.tyre[t], &r, now);
        track(t);
        s.rssi[t] = p->rssi;
        note_packet(t, &r, p->rssi, now);
        if (s.logged_at[t] == 0 || now - s.logged_at[t] >= LOG_MS) {
            s.logged_at[t] = now;
            log_reading(TPMS_TYRE_NAMES[t], &r, &s.pair[t], p, now);
        }
    }
    if (!mine && s.pairing < 0) log_seen(&r, p, now);
}

// ---- Commands -----------------------------------------------------------------------

static void pick(int t, const tpms_pairing_t *sensor, int64_t now)
{
    if (t < 0 || t >= TPMS_TYRES || sensor->format == TPMS_F_NONE) return;
    // A sensor sits on one tyre only.
    for (int o = 0; o < TPMS_TYRES; o++) {
        if (o != t && tpms_pairing_same(&s.pair[o], sensor)) {
            memset(&s.pair[o], 0, sizeof(s.pair[o]));
            tpms_tyre_init(&s.tyre[o]);
            s.rssi[o] = 0;
        }
    }
    s.pair[t] = *sensor;
    tpms_tyre_init(&s.tyre[t]);
    s.rssi[t] = 0;
    s.logged_at[t] = 0;
    // Show the reading pairing heard right away.
    for (int i = 0; i < s.cands.n; i++) {
        const tpms_cand_t *c = &s.cands.c[i];
        if (tpms_pairing_same(&c->sensor, sensor)) {
            tpms_tyre_update(&s.tyre[t], &c->reading, c->at);
            s.rssi[t] = c->rssi;
        }
    }
    s.pairing = -1;
    save();
    char tail[7];
    tail_of(sensor, tail);
    ESP_LOGI(TAG, "%s tyre paired: %s %s", TPMS_TYRE_NAMES[t], TPMS_FORMAT_NAMES[sensor->format], tail);
    char note[80];
    snprintf(note, sizeof(note), "tpms %s paired: %s %s", TPMS_TYRE_NAMES[t], TPMS_FORMAT_NAMES[sensor->format], tail);
    sd_log_note(note, now);
}

static void run_command(const cmd_t *c, int64_t now)
{
    switch (c->type) {
    case C_PAIR_START:
        if (c->tyre < 0 || c->tyre >= TPMS_TYRES) break;
        s.pairing = c->tyre;
        s.pair_until = now + TPMS_PAIR_MS;
        tpms_cands_clear(&s.cands);
        ESP_LOGI(TAG, "pairing the %s tyre: listening", TPMS_TYRE_NAMES[c->tyre]);
        break;
    case C_PAIR_STOP:
        s.pairing = -1;
        break;
    case C_PAIR_PICK:
        if (s.demo) {
            s.pairing = -1;   // demo sensors aren't saved
            break;
        }
        pick(c->tyre, &c->sensor, now);
        break;
    case C_FORGET:
        if (c->tyre < 0 || c->tyre >= TPMS_TYRES || s.demo) break;
        memset(&s.pair[c->tyre], 0, sizeof(s.pair[c->tyre]));
        tpms_tyre_init(&s.tyre[c->tyre]);
        s.rssi[c->tyre] = 0;
        save();
        ESP_LOGI(TAG, "%s tyre's sensor forgotten", TPMS_TYRE_NAMES[c->tyre]);
        break;
    case C_SET_CFG:
        s.cfg = c->cfg;
        s.cfg.version = TPMS_CFG_VERSION;
        save();
        break;
    }
}

static void command(cmd_type_t type, int tyre, const tpms_pairing_t *sensor, const tpms_cfg_t *cfg)
{
    if (!s_cmds) return;
    cmd_t c = {.type = type, .tyre = tyre};
    if (sensor) c.sensor = *sensor;
    if (cfg) c.cfg = *cfg;
    xQueueSend(s_cmds, &c, pdMS_TO_TICKS(100));
}

void tpms_service_pair_start(int tyre) { command(C_PAIR_START, tyre, NULL, NULL); }
void tpms_service_pair_stop(void) { command(C_PAIR_STOP, -1, NULL, NULL); }
void tpms_service_pair_pick(int tyre, const tpms_pairing_t *sensor) { command(C_PAIR_PICK, tyre, sensor, NULL); }
void tpms_service_forget(int tyre) { command(C_FORGET, tyre, NULL, NULL); }
void tpms_service_set_cfg(const tpms_cfg_t *cfg) { command(C_SET_CFG, -1, NULL, cfg); }

// ---- Demo mode ------------------------------------------------------------------------

// Synthetic tyres on a 120 s loop, at the bike profile's pressures: both warm
// up; from 60 s the rear leaks, below its default low alert about 17 s later
// and fast enough for the fast-loss alert; after 95 s the front runs hot. The front reports a battery percentage, the rear a falling voltage.
// Pairing lists them and a stranger; nothing in demo is saved.
static const tpms_pairing_t DEMO_PAIR[TPMS_TYRES] = {
    {.format = TPMS_F_DJTPMS, .id_len = 6, .id = {0xD0, 0xE3, 0x00, 0x00, 0xF2, 0x01}},
    {.format = TPMS_F_DJTPMS, .id_len = 6, .id = {0xD0, 0xE3, 0x00, 0x00, 0xA2, 0x02}},
};
static const tpms_pairing_t DEMO_STRANGER = {.format = TPMS_F_SYTPMS, .addr = {0x33, 0x22, 0x11, 0x85, 0x15, 0xAC}};

static tpms_reading_t demo_reading(float kpa, float c, int pct, float volts)
{
    tpms_reading_t r = {.format = TPMS_F_DJTPMS, .position = -1, .kpa = kpa, .temp_c = c, .volts = volts,
                        .battery_pct = pct, .awake = true};
    return r;
}

static void demo_feed(int64_t now)
{
    static int64_t last;
    if (now - last < 1000) return;
    last = now;
    float t = (float)((now / 1000) % 120);
    float rear = t < 60 ? BIKE_TYRE_REAR_KPA : BIKE_TYRE_REAR_KPA - (t - 60) * 1.5f;
    tpms_reading_t f = demo_reading(BIKE_TYRE_FRONT_KPA, 18.0f + t * 0.2f + (t > 95 ? 45.0f : 0.0f), 80, NAN);
    tpms_reading_t r = demo_reading(rear, 18.0f + t * 0.15f, -1, 2.9f - t * 0.004f);
    tpms_tyre_update(&s.tyre[TPMS_FRONT], &f, now);
    tpms_tyre_update(&s.tyre[TPMS_REAR], &r, now);
    track(TPMS_FRONT);
    track(TPMS_REAR);
    s.rssi[TPMS_FRONT] = -58;
    s.rssi[TPMS_REAR] = -71;
    if (s.pairing >= 0) {
        static const uint8_t none[6] = {0};
        tpms_reading_t x = f;
        x.format = TPMS_F_SYTPMS;
        x.id_len = 0;
        x.kpa = 0.0f;
        memcpy(f.id, DEMO_PAIR[0].id, 6), f.id_len = 6;
        memcpy(r.id, DEMO_PAIR[1].id, 6), r.id_len = 6;
        tpms_cands_offer(&s.cands, &f, none, s.pairing == TPMS_FRONT ? -48 : -66, now);
        tpms_cands_offer(&s.cands, &r, none, s.pairing == TPMS_REAR ? -50 : -69, now);
        tpms_cands_offer(&s.cands, &x, DEMO_STRANGER.addr, -88, now);
    }
}

// ---- The task's side ------------------------------------------------------------------

void tpms_service_init(void)
{
    memset(&s, 0, sizeof(s));
    s.pairing = -1;
    tpms_alerts_init(&s.alerts);
    ride_reset();
    for (int t = 0; t < TPMS_TYRES; t++) tpms_tyre_init(&s.tyre[t]);
    if (!nvs_get_blob_("tpms_cfg", &s.cfg, sizeof(s.cfg)) || s.cfg.version != TPMS_CFG_VERSION) {
        tpms_cfg_defaults(&s.cfg);
    }
    if (!nvs_get_blob_("tpms_pair", s.pair, sizeof(s.pair))) {
        memset(s.pair, 0, sizeof(s.pair));
    }
    for (int t = 0; t < TPMS_TYRES; t++) {
        if (s.pair[t].format >= TPMS_F_COUNT) memset(&s.pair[t], 0, sizeof(s.pair[t]));
    }
    s_pkts = xQueueCreate(PKT_QUEUE, sizeof(pkt_t));
    s_cmds = xQueueCreate(CMD_QUEUE, sizeof(cmd_t));
    ble_scan_set_tpms_sink(on_adv);
    update_want_scan();
    char f[7], r[7];
    tail_of(&s.pair[0], f);
    tail_of(&s.pair[1], r);
    ESP_LOGI(TAG, "front %s %s, rear %s %s", TPMS_FORMAT_NAMES[s.pair[0].format], paired(0) ? f : "-",
             TPMS_FORMAT_NAMES[s.pair[1].format], paired(1) ? r : "-");
}

void tpms_service_poll(int64_t now, bool demo)
{
    if (demo != s.demo) {
        s.demo = demo;
        s.pairing = -1;
        tpms_alerts_init(&s.alerts);
        ride_reset();
        for (int t = 0; t < TPMS_TYRES; t++) {
            tpms_tyre_init(&s.tyre[t]);
            s.rssi[t] = 0;
        }
    }
    cmd_t c;
    while (xQueueReceive(s_cmds, &c, 0) == pdTRUE) run_command(&c, now);
    pkt_t p;
    while (xQueueReceive(s_pkts, &p, 0) == pdTRUE) {
        if (!demo) handle(&p, now);
    }
    if (demo) demo_feed(now);
    if (s.pairing >= 0 && now >= s.pair_until) {
        ESP_LOGI(TAG, "pairing stopped: %d sensors heard", s.cands.n);
        s.pairing = -1;
    }
    check_card(now);
    update_want_scan();
}

void tpms_service_publish(tpms_view_t *v, int64_t now)
{
    v->on = app_settings()->tpms;
    v->demo = s.demo;
    memcpy(v->pair, s.demo ? DEMO_PAIR : s.pair, sizeof(v->pair));
    memcpy(v->tyre, s.tyre, sizeof(v->tyre));
    memcpy(v->rssi, s.rssi, sizeof(v->rssi));
    memcpy(v->ride_min_kpa, s.ride_min_kpa, sizeof(v->ride_min_kpa));
    memcpy(v->ride_max_kpa, s.ride_max_kpa, sizeof(v->ride_max_kpa));
    memcpy(v->ride_max_c, s.ride_max_c, sizeof(v->ride_max_c));
    memcpy(v->ride_heard, s.ride_heard, sizeof(v->ride_heard));
    memcpy(v->ride_min_rssi, s.ride_min_rssi, sizeof(v->ride_min_rssi));
    memcpy(v->reading_seq, s.reading_seq, sizeof(v->reading_seq));
    memcpy(v->reading_kpa, s.reading_kpa, sizeof(v->reading_kpa));
    memcpy(v->reading_c, s.reading_c, sizeof(v->reading_c));
    memcpy(v->reading_rssi, s.reading_rssi, sizeof(v->reading_rssi));
    v->cfg = s.cfg;
    v->pairing = s.pairing;
    v->pair_left_s = s.pairing >= 0 ? (int)((s.pair_until - now + 999) / 1000) : 0;
    int idx[TPMS_CAND_MAX];
    int n = s.pairing >= 0 ? tpms_cands_fresh(&s.cands, now, idx) : 0;
    for (int i = 0; i < n; i++) {
        const tpms_cand_t *c = &s.cands.c[idx[i]];
        tpms_cand_view_t *cv = &v->cands[i];
        cv->sensor = c->sensor;
        cv->kpa = c->reading.kpa;
        cv->rssi = c->rssi;
        cv->on_tyre = -1;
        for (int t = 0; t < TPMS_TYRES; t++) {
            if (tpms_pairing_same(s.demo ? &DEMO_PAIR[t] : &s.pair[t], &c->sensor)) cv->on_tyre = t;
        }
    }
    v->n_cands = n;
}

tpms_alert_t tpms_service_alert(int64_t now, bool riding, int *tyre)
{
    bool on[TPMS_TYRES];
    for (int t = 0; t < TPMS_TYRES; t++) on[t] = s.demo || (app_settings()->tpms && paired(t));
    tpms_alert_t a = tpms_alerts_evaluate(&s.alerts, &s.cfg, s.tyre, on, now, riding);
    *tyre = s.alerts.slot;
    if (a != TPMS_A_NONE) {
        const tpms_tyre_t *y = &s.tyre[s.alerts.slot];
        char note[96];
        snprintf(note, sizeof(note), "tpms alert %d on the %s tyre: %.0f kPa %.0f C", a, TPMS_TYRE_NAMES[s.alerts.slot],
                 y->kpa, y->temp_c);
        ESP_LOGW(TAG, "%s", note + 5);
        sd_log_note(note, now);
    }
    return a;
}

void tpms_service_new_ride(void)
{
    for (int t = 0; t < TPMS_TYRES; t++) s.alerts.armed[t][TPMS_A_BATTERY] = true;
    ride_reset();
    for (int t = 0; t < TPMS_TYRES; t++) track(t);   // the tyres as they stand at the start
}

bool tpms_service_wants_scan(void)
{
    return s_want_scan;
}
