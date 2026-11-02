/*
 * Page 3, Tyres: a line drawing of a motorcycle, facing right
 * (tools/make_tyre_bike.py), whose wheels are the gauges: each is a ring in its tyre's
 * colour with the pressure inside, the rear on the left. Under each wheel,
 * its name, then its temperature and a small battery icon for its sensor. A ring turns amber near the tyre's low alert or when the
 * sensor goes quiet, and red past a limit. Tap a wheel for its details and
 * alert limits; long-press it to pair its sensor. The page's hold (long press
 * elsewhere, or holding BOOT) pairs the front, then the rear.
 * The values come from tpms_service.c through dash_t.tpms.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include "ui_common.h"
#include "assets/tyre_bike.h"
#include "tpms_service.h"

#define NEAR_KPA    10.0f        // amber this close above the low alert
#define UPDATE_MS   250

#define BIKE_X      ((UI_W - TYRE_BIKE_W) / 2)
#define BIKE_Y      72           // the bike and the lines under it, centred in the circle
#define RING_W      6
#define RING_OVER   2            // a ring this much bigger than its tyre hides the tread's edge
#define INFO_Y      (BIKE_Y + TYRE_BIKE_FRONT_Y + TYRE_BIKE_FRONT_R + RING_OVER + 8)   // the first line under the wheels
#define BATT_FILL_W 18           // the sensor battery icon, as the board's (ui.c)

typedef struct {
    lv_obj_t *ring, *value, *unit, *name, *temp;
    lv_obj_t *batt, *batt_body, *batt_nub, *batt_fill;
} tyre_ui_t;

static tyre_ui_t s_t[TPMS_TYRES];
static lv_obj_t *s_cap, *s_hint;   // the hint takes the caption's place when it has something to say
static int64_t s_last;
static EXT_RAM_BSS_ATTR tpms_view_t s_v;   // the last view, for the sheets' actions
static int64_t s_now;

static tpms_unit_t unit(void) { return (tpms_unit_t)app_settings()->pressure_unit; }

static bool fresh(const tpms_view_t *v, int t, int64_t now)
{
    int64_t age = tpms_tyre_age(&v->tyre[t], now);
    return v->pair[t].format && age >= 0 && age < TPMS_STALE_MS;
}

// The tyre's colour: red past a limit, amber near the low alert or quiet, green otherwise.
static lv_color_t tyre_color(const tpms_view_t *v, int t, int64_t now)
{
    if (!v->pair[t].format) return C_FAINT;
    if (!fresh(v, t, now)) return C_DIM;
    const tpms_tyre_t *y = &v->tyre[t];
    const tpms_cfg_t *c = &v->cfg;
    bool low = c->low_on[t] && y->kpa < c->low_kpa[t];
    bool high = c->high_on[t] && y->kpa > c->high_kpa[t];
    bool hot = c->hot_on && !isnan(y->temp_c) && y->temp_c > c->hot_c;
    if (low || high || hot || (c->sensor_alarm && y->alarm)) return C_ALERT;
    if (c->low_on[t] && y->kpa < c->low_kpa[t] + NEAR_KPA) return C_WARN;
    return C_OK;
}

static void fmt_p(char *buf, size_t n, float kpa)
{
    tpms_fmt_pressure(buf, n, kpa, unit());
}

static void fmt_age(char *buf, size_t n, int64_t ms)
{
    if (ms < 60000) snprintf(buf, n, "%d s", (int)(ms / 1000));
    else snprintf(buf, n, "%d min", (int)(ms / 60000));
}

// ---- Pairing sheet --------------------------------------------------------------

#define ROWS TPMS_CAND_MAX

static int s_pair_tyre = -1;     // the tyre the open pairing sheet is for
static bool s_pair_chain;        // the page's hold: pair the rear after the front
// Each row: the id tail over its format on the left, the pressure over its
// signal (and the tyre it is on) on the right.
static lv_obj_t *s_pair_status, *s_pair_rows[ROWS], *s_pair_id[ROWS], *s_pair_fmt[ROWS], *s_pair_p[ROWS],
    *s_pair_sig[ROWS], *s_pair_again;
static EXT_RAM_BSS_ATTR tpms_cand_view_t s_pair_shown[ROWS];
static int s_pair_n;
static int64_t s_pair_since;     // the service picks the command up within a tick or two

static void open_pairing(int t);

static void on_pair_row(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i >= s_pair_n || s_pair_tyre < 0) return;
    ui_tick_feedback();
    int t = s_pair_tyre;
    tpms_service_pair_pick(t, &s_pair_shown[i].sensor);
    char tail[7], msg[48];
    tpms_pairing_tail(&s_pair_shown[i].sensor, tail);
    snprintf(msg, sizeof(msg), "%s: %s paired", t == TPMS_FRONT ? "Front" : "Rear", tail);
    s_pair_tyre = -1;
    ui_sheet_close();
    ui_toast(msg);
    if (s_pair_chain && t == TPMS_FRONT) {
        open_pairing(TPMS_REAR);
    } else {
        s_pair_chain = false;
    }
}

static void on_pair_again(lv_event_t *e)
{
    ui_tick_feedback();
    s_pair_since = s_now;
    tpms_service_pair_start(s_pair_tyre);
}

static void on_pair_cancel(lv_event_t *e)
{
    ui_tick_feedback();
    tpms_service_pair_stop();
    s_pair_tyre = -1;
    s_pair_chain = false;
    ui_sheet_close();
}

static lv_obj_t *button(lv_obj_t *col, const char *text, lv_color_t color, lv_event_cb_t cb, void *arg)
{
    lv_obj_t *btn = lv_button_create(col);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 200, 40);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, C_FAINT, 0);
    lv_obj_set_style_bg_color(btn, C_TRACK_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_margin_top(btn, 6, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, arg);
    lv_obj_t *bt = ui_label(btn, &font_txt_18, color, text);
    lv_obj_center(bt);
    return btn;
}

static void open_pairing(int t)
{
    if (!app_settings()->tpms && !s_v.demo) {
        ui_toast("Tyre sensors are off (Status)");
        s_pair_chain = false;
        return;
    }
    s_pair_tyre = t;
    s_pair_n = 0;
    s_pair_since = s_now;
    tpms_service_pair_start(t);
    lv_obj_t *col = ui_sheet();
    lv_obj_t *cap = ui_label(col, &font_txt_14, C_ACCENT, t == TPMS_FRONT ? "PAIR FRONT" : "PAIR REAR");
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    lv_obj_t *hint = ui_label(col, &font_txt_14, C_DIM,
                              "Strongest first. Hold the board by the\n"
                              "valve, or spin the wheel to wake the sensor.\n"
                              "The id matches the one the sensor's app shows.");
    lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
    s_pair_status = ui_label(col, &font_txt_14, C_TEXT, "Listening\xE2\x80\xA6");
    for (int i = 0; i < ROWS; i++) {
        lv_obj_t *b = lv_button_create(col);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 250, 54);
        lv_obj_set_style_radius(b, 14, 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_border_color(b, C_TRACK_HI, 0);
        lv_obj_set_style_bg_color(b, C_TRACK_HI, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_event_cb(b, on_pair_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_pair_id[i] = ui_label(b, &font_txt_18, C_TEXT, "");
        lv_obj_align(s_pair_id[i], LV_ALIGN_TOP_LEFT, 14, 6);
        s_pair_fmt[i] = ui_label(b, &font_txt_14, C_DIM, "");
        lv_obj_align(s_pair_fmt[i], LV_ALIGN_BOTTOM_LEFT, 14, -6);
        s_pair_p[i] = ui_label(b, &font_txt_18, C_TEXT, "");
        lv_obj_align(s_pair_p[i], LV_ALIGN_TOP_RIGHT, -14, 6);
        s_pair_sig[i] = ui_label(b, &font_txt_14, C_DIM, "");
        lv_obj_align(s_pair_sig[i], LV_ALIGN_BOTTOM_RIGHT, -14, -6);
        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        s_pair_rows[i] = b;
    }
    s_pair_again = button(col, "Listen again", C_ACCENT, on_pair_again, NULL);
    lv_obj_add_flag(s_pair_again, LV_OBJ_FLAG_HIDDEN);
    button(col, "Cancel", C_DIM, on_pair_cancel, NULL);
}

// Whether the sheet holding o is still open: no ancestor hidden. (Not
// lv_obj_is_visible, which is false while the list is scrolled past o.)
static bool still_open(lv_obj_t *o)
{
    if (!lv_obj_is_valid(o)) return false;
    for (; o; o = lv_obj_get_parent(o)) {
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return false;
    }
    return true;
}

static void update_pairing(const tpms_view_t *v)
{
    if (s_pair_tyre < 0) return;
    if (!still_open(s_pair_status)) {
        // Closed with BOOT or by another dialog: stop listening.
        s_pair_tyre = -1;
        s_pair_chain = false;
        tpms_service_pair_stop();
        return;
    }
    char buf[64];
    bool listening = v->pairing == s_pair_tyre || s_now - s_pair_since < 1000;
    if (listening) {
        snprintf(buf, sizeof(buf), "Listening\xE2\x80\xA6 %d s", v->pair_left_s);
    } else {
        snprintf(buf, sizeof(buf), v->n_cands || s_pair_n ? "Stopped listening" :
                 "Nothing heard: spin the wheel, or\nunscrew and refit the cap");
    }
    ui_label_set(s_pair_status, buf);
    if (listening) lv_obj_add_flag(s_pair_again, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(s_pair_again, LV_OBJ_FLAG_HIDDEN);
    if (!listening) return;   // keep the last list to pick from
    s_pair_n = v->n_cands;
    memcpy(s_pair_shown, v->cands, sizeof(s_pair_shown));
    for (int i = 0; i < ROWS; i++) {
        if (i >= s_pair_n) {
            lv_obj_add_flag(s_pair_rows[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const tpms_cand_view_t *c = &s_pair_shown[i];
        char tail[7], p[12];
        tpms_pairing_tail(&c->sensor, tail);
        ui_label_set(s_pair_id[i], tail);
        ui_label_set(s_pair_fmt[i], TPMS_FORMAT_NAMES[c->sensor.format]);
        if (isnan(c->kpa)) snprintf(p, sizeof(p), "--");
        else fmt_p(p, sizeof(p), c->kpa);
        snprintf(buf, sizeof(buf), "%s %s", p, tpms_unit_name(unit()));
        ui_label_set(s_pair_p[i], buf);
        snprintf(buf, sizeof(buf), "%s%d dB", c->on_tyre < 0 ? "" : c->on_tyre == TPMS_FRONT ? "on front  " : "on rear  ",
                 c->rssi);
        ui_label_set(s_pair_sig[i], buf);
        lv_obj_remove_flag(s_pair_rows[i], LV_OBJ_FLAG_HIDDEN);
    }
}

// ---- A tyre's sheet: details and alert limits ------------------------------------

static int s_edit_tyre;
static bool s_edit_high;
static lv_obj_t *s_low_val, *s_high_val, *s_low_sw, *s_high_sw;

static void kv(lv_obj_t *col, const char *caption, const char *value, lv_color_t color)
{
    lv_obj_t *r = ui_box(col, 250, LV_SIZE_CONTENT);
    lv_obj_t *c = ui_label(r, &font_txt_14, C_DIM, caption);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *v = ui_label(r, &font_txt_14, color, value);
    lv_obj_set_width(v, 160);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 0);
}

static void limit_text(char *buf, size_t n, float kpa)
{
    char p[12];
    fmt_p(p, sizeof(p), kpa);
    snprintf(buf, n, "%s %s", p, tpms_unit_name(unit()));
}

static void save_limit(float v)
{
    tpms_cfg_t c = s_v.cfg;
    int t = s_edit_tyre;
    float kpa = tpms_to_kpa(v, unit());
    char buf[24], msg[64];
    limit_text(buf, sizeof(buf), kpa);
    if (s_edit_high) {
        c.high_kpa[t] = kpa;
        c.high_on[t] = true;
    } else {
        c.low_kpa[t] = kpa;
        c.low_on[t] = true;
    }
    s_v.cfg = c;
    tpms_service_set_cfg(&c);
    lv_obj_t *val = s_edit_high ? s_high_val : s_low_val, *sw = s_edit_high ? s_high_sw : s_low_sw;
    if (lv_obj_is_valid(val)) ui_label_set(val, buf);
    if (lv_obj_is_valid(sw)) lv_obj_add_state(sw, LV_STATE_CHECKED);
    snprintf(msg, sizeof(msg), "%s %s alert: %s", t == TPMS_FRONT ? "Front" : "Rear", s_edit_high ? "high" : "low",
             buf);
    ui_toast(msg);
}

static void on_limit(lv_event_t *e)
{
    s_edit_high = (bool)(intptr_t)lv_event_get_user_data(e);
    ui_tick_feedback();
    const tpms_cfg_t *c = &s_v.cfg;
    float kpa = s_edit_high ? c->high_kpa[s_edit_tyre] : c->low_kpa[s_edit_tyre];
    tpms_unit_t u = unit();
    char title[32];
    snprintf(title, sizeof(title), "%s %s alert", s_edit_tyre == TPMS_FRONT ? "Front" : "Rear",
             s_edit_high ? "high" : "low");
    ui_number_picker(title, s_edit_high ? "Warn me above this pressure" : "Warn me below this pressure",
                     u == TPMS_UNIT_BAR ? 6 : u == TPMS_UNIT_PSI ? 90 : 600, u != TPMS_UNIT_KPA, tpms_unit_name(u),
                     tpms_from_kpa(kpa, u), save_limit);
}

static void on_limit_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    tpms_cfg_t c = s_v.cfg;
    if (sw == s_high_sw) c.high_on[s_edit_tyre] = on;
    else c.low_on[s_edit_tyre] = on;
    s_v.cfg = c;
    tpms_service_set_cfg(&c);
    ui_tick_feedback();
}

static lv_obj_t *limit_row(lv_obj_t *col, const char *caption, bool high, float kpa, bool on, lv_obj_t **value,
                           lv_obj_t **sw)
{
    lv_obj_t *r = ui_box(col, 250, 40);
    lv_obj_t *c = ui_label(r, &font_txt_18, C_TEXT, caption);
    lv_obj_align(c, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *btn = lv_button_create(r);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 92, 32);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_bg_color(btn, C_TRACK_HI, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, C_FAINT, LV_STATE_PRESSED);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, -60, 0);
    lv_obj_add_event_cb(btn, on_limit, LV_EVENT_CLICKED, (void *)(intptr_t)high);
    char buf[24];
    limit_text(buf, sizeof(buf), kpa);
    *value = ui_label(btn, &font_txt_14, C_ACCENT, buf);
    lv_obj_center(*value);
    *sw = lv_switch_create(r);
    lv_obj_set_size(*sw, 50, 28);
    lv_obj_align(*sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(*sw, C_TRACK_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_color(*sw, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) lv_obj_add_state(*sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(*sw, on_limit_switch, LV_EVENT_VALUE_CHANGED, NULL);
    return r;
}

static void on_pair_from_sheet(lv_event_t *e)
{
    ui_tick_feedback();
    s_pair_chain = false;
    open_pairing(s_edit_tyre);
}

static void forget_sensor(void)
{
    tpms_service_forget(s_edit_tyre);
    ui_toast(s_edit_tyre == TPMS_FRONT ? "Front sensor forgotten" : "Rear sensor forgotten");
}

static void on_forget(lv_event_t *e)
{
    ui_tick_feedback();
    ui_confirm(s_edit_tyre == TPMS_FRONT ? "Forget the front sensor?" : "Forget the rear sensor?",
               "Pair it again to see this tyre.", "Forget", C_WARN, forget_sensor);
}

static void on_back(lv_event_t *e)
{
    ui_tick_feedback();
    ui_sheet_close();
}

static void show_tyre(int t)
{
    const tpms_view_t *v = &s_v;
    const tpms_tyre_t *y = &v->tyre[t];
    s_edit_tyre = t;
    lv_obj_t *col = ui_sheet();
    lv_obj_t *cap = ui_label(col, &font_txt_14, C_ACCENT, t == TPMS_FRONT ? "FRONT TYRE" : "REAR TYRE");
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    char buf[64], p[12];
    bool paired = v->pair[t].format != TPMS_F_NONE;
    bool ok = fresh(v, t, s_now);
    if (ok && !isnan(y->kpa)) {
        fmt_p(p, sizeof(p), y->kpa);
        snprintf(buf, sizeof(buf), "%s %s", p, tpms_unit_name(unit()));
    } else {
        snprintf(buf, sizeof(buf), paired ? "no signal" : "no sensor");
    }
    ui_label(col, &font_num_48, paired ? tyre_color(v, t, s_now) : C_DIM, buf);
    if (paired) {
        char tail[7];
        tpms_pairing_tail(&v->pair[t], tail);
        snprintf(buf, sizeof(buf), "%s  %s", TPMS_FORMAT_NAMES[v->pair[t].format], tail);
        kv(col, "Sensor", buf, C_TEXT);
        snprintf(buf, sizeof(buf), isnan(y->temp_c) ? "--" : "%.0f \xC2\xB0" "C", y->temp_c);
        kv(col, "Temperature", buf, C_TEXT);
        int lvl = tpms_battery_level(y->battery_pct, y->volts);
        if (lvl < 0) snprintf(buf, sizeof(buf), "--");
        else if (!isnan(y->volts)) snprintf(buf, sizeof(buf), "%d%%  %.2f V", lvl, y->volts);
        else snprintf(buf, sizeof(buf), "%d%%", lvl);
        kv(col, "Sensor battery", buf, tpms_battery_low(y->battery_pct, y->volts) ? C_ALERT : C_TEXT);
        if (y->heard_at >= 0) {
            fmt_age(p, sizeof(p), s_now - y->heard_at);
            snprintf(buf, sizeof(buf), "%s ago  %d dB", p, v->rssi[t]);
        } else {
            snprintf(buf, sizeof(buf), "not yet");
        }
        kv(col, "Last heard", buf, C_TEXT);
        if (!isnan(v->ride_min_kpa[t])) {
            char lo[12], hi[12];
            fmt_p(lo, sizeof(lo), v->ride_min_kpa[t]);
            fmt_p(hi, sizeof(hi), v->ride_max_kpa[t]);
            int n = snprintf(buf, sizeof(buf), "%s\xE2\x80\x93%s", lo, hi);
            if (!isnan(v->ride_max_c[t])) snprintf(buf + n, sizeof(buf) - n, ", max %.0f\xC2\xB0", v->ride_max_c[t]);
            kv(col, "This ride", buf, C_TEXT);
        }
    }
    limit_row(col, "Low alert", false, v->cfg.low_kpa[t], v->cfg.low_on[t], &s_low_val, &s_low_sw);
    limit_row(col, "High alert", true, v->cfg.high_kpa[t], v->cfg.high_on[t], &s_high_val, &s_high_sw);
    if (v->demo) ui_label(col, &font_txt_14, C_DIM, "Demo tyres: pairing isn't saved");
    button(col, paired ? "Pair another sensor" : "Pair sensor", C_ACCENT, on_pair_from_sheet, NULL);
    if (paired && !v->demo) button(col, "Forget sensor", C_WARN, on_forget, NULL);
    button(col, "Back", C_DIM, on_back, NULL);
}

// ---- The page ---------------------------------------------------------------------

static void on_pill(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    ui_tick_feedback();
    show_tyre(t);
}

static void on_pill_long(lv_event_t *e)
{
    int t = (int)(intptr_t)lv_event_get_user_data(e);
    ui_tick_feedback();
    s_pair_chain = false;
    open_pairing(t);
}

static void hold(void)
{
    ui_tick_feedback();
    s_pair_chain = true;
    open_pairing(TPMS_FRONT);
}

static void on_long_press(lv_event_t *e) { hold(); }

static lv_obj_t *info(lv_obj_t *tile, const lv_font_t *f, lv_color_t c, int x, int y)
{
    lv_obj_t *l = ui_label(tile, f, c, "");
    lv_obj_set_width(l, 130);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_TOP_LEFT, x - 65, y);
    return l;
}

// A battery outline with a nub and a fill, 27 x 12, like the board's.
static void build_battery(lv_obj_t *parent, tyre_ui_t *u)
{
    u->batt = ui_box(parent, 27, 12);
    u->batt_body = ui_box(u->batt, 24, 12);
    lv_obj_align(u->batt_body, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(u->batt_body, 3, 0);
    lv_obj_set_style_border_width(u->batt_body, 2, 0);
    lv_obj_set_style_border_color(u->batt_body, C_DIM, 0);
    u->batt_nub = ui_box(u->batt, 2, 5);
    lv_obj_align(u->batt_nub, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(u->batt_nub, 1, 0);
    lv_obj_set_style_bg_opa(u->batt_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(u->batt_nub, C_DIM, 0);
    u->batt_fill = ui_box(u->batt_body, BATT_FILL_W, 6);
    lv_obj_align(u->batt_fill, LV_ALIGN_LEFT_MID, 3, 0);
    lv_obj_set_style_radius(u->batt_fill, 1, 0);
    lv_obj_set_style_bg_opa(u->batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(u->batt_fill, C_OK, 0);
    lv_obj_add_flag(u->batt, LV_OBJ_FLAG_HIDDEN);
}

static void build_tyre(lv_obj_t *tile, int t, int cx, int cy, int r)
{
    tyre_ui_t *u = &s_t[t];
    u->ring = lv_button_create(tile);
    lv_obj_remove_style_all(u->ring);
    lv_obj_set_size(u->ring, 2 * r, 2 * r);
    lv_obj_set_pos(u->ring, cx - r, cy - r);
    lv_obj_set_style_radius(u->ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(u->ring, RING_W, 0);
    lv_obj_set_style_border_color(u->ring, C_FAINT, 0);
    lv_obj_set_style_bg_color(u->ring, C_TRACK, 0);
    lv_obj_set_style_bg_opa(u->ring, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(u->ring, C_TRACK_HI, LV_STATE_PRESSED);
    lv_obj_add_event_cb(u->ring, on_pill, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)t);
    lv_obj_add_event_cb(u->ring, on_pill_long, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)t);
    u->value = ui_label(u->ring, &font_num_32, C_TEXT, "--");
    lv_obj_align(u->value, LV_ALIGN_CENTER, 0, -7);
    u->unit = ui_label(u->ring, &font_txt_14, C_DIM, "");
    lv_obj_align(u->unit, LV_ALIGN_CENTER, 0, 17);

    u->name = info(tile, &font_txt_14, C_DIM, cx, INFO_Y);
    lv_obj_set_style_text_letter_space(u->name, 2, 0);
    ui_label_set(u->name, t == TPMS_FRONT ? "FRONT" : "REAR");
    // The temperature and the sensor's battery, side by side under the name.
    lv_obj_t *row = ui_box(tile, 130, 24);
    lv_obj_set_pos(row, cx - 65, INFO_Y + 18);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 8, 0);
    u->temp = ui_label(row, &font_txt_18, C_TEXT, "");
    build_battery(row, u);
}

static void create(lv_obj_t *tile)
{
    lv_obj_add_event_cb(tile, on_long_press, LV_EVENT_LONG_PRESSED, NULL);
    s_cap = ui_label(tile, &font_txt_14, C_DIM, "TYRES");
    lv_obj_set_style_text_letter_space(s_cap, 3, 0);
    lv_obj_align(s_cap, LV_ALIGN_CENTER, 0, -134);
    lv_obj_t *bike = lv_image_create(tile);
    lv_image_set_src(bike, &tyre_bike);
    lv_obj_set_pos(bike, BIKE_X, BIKE_Y);
    // The rings sit on the photo's tyres: the 19" front is the bigger one.
    build_tyre(tile, TPMS_REAR, BIKE_X + TYRE_BIKE_REAR_X, BIKE_Y + TYRE_BIKE_REAR_Y, TYRE_BIKE_REAR_R + RING_OVER);
    build_tyre(tile, TPMS_FRONT, BIKE_X + TYRE_BIKE_FRONT_X, BIKE_Y + TYRE_BIKE_FRONT_Y, TYRE_BIKE_FRONT_R + RING_OVER);
    s_hint = ui_label(tile, &font_txt_14, C_DIM, "");
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, -134);
}

// The sensor battery icon: hidden when the sensor sends no level; green, or
// red with a red outline when it is low.
static void update_battery(tyre_ui_t *u, const tpms_tyre_t *y, bool show)
{
    int lvl = show ? tpms_battery_level(y->battery_pct, y->volts) : -1;
    if (lvl < 0) {
        lv_obj_add_flag(u->batt, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(u->batt, LV_OBJ_FLAG_HIDDEN);
    int w = lvl * BATT_FILL_W / 100;
    if (w < 2) w = 2;   // a sliver at 0 %, so it still reads as a level
    if (lv_obj_get_width(u->batt_fill) != w) lv_obj_set_width(u->batt_fill, w);
    bool low = tpms_battery_low(y->battery_pct, y->volts);
    lv_color_t fill = low ? C_ALERT : C_OK, edge = low ? C_ALERT : C_DIM;
    if (!lv_color_eq(lv_obj_get_style_bg_color(u->batt_fill, 0), fill)) lv_obj_set_style_bg_color(u->batt_fill, fill, 0);
    if (!lv_color_eq(lv_obj_get_style_border_color(u->batt_body, 0), edge)) {
        lv_obj_set_style_border_color(u->batt_body, edge, 0);
        lv_obj_set_style_bg_color(u->batt_nub, edge, 0);
    }
}

static void update_tyre(const tpms_view_t *v, int t, int64_t now)
{
    tyre_ui_t *u = &s_t[t];
    const tpms_tyre_t *y = &v->tyre[t];
    lv_color_t col = tyre_color(v, t, now);
    lv_obj_set_style_border_color(u->ring, col, 0);
    char buf[40];
    if (!v->pair[t].format) {
        ui_label_set(u->value, "--");
        ui_label_color(u->value, C_FAINT);
        ui_label_set(u->unit, "");
        ui_label_set(u->temp, "Tap to pair");
        ui_label_color(u->temp, C_DIM);
        update_battery(u, y, false);
        return;
    }
    bool ok = fresh(v, t, now);
    if (ok && !isnan(y->kpa)) fmt_p(buf, sizeof(buf), y->kpa);
    else snprintf(buf, sizeof(buf), "--");
    ui_label_set(u->value, buf);
    ui_label_color(u->value, ok ? (lv_color_eq(col, C_OK) ? C_TEXT : col) : C_DIM);
    ui_label_set(u->unit, tpms_unit_name(unit()));
    // The temperature; while the sensor is quiet, why there's no reading.
    if (ok && !isnan(y->temp_c)) snprintf(buf, sizeof(buf), "%.0f\xC2\xB0" "C", y->temp_c);
    else if (ok) snprintf(buf, sizeof(buf), "--");
    else if (tpms_tyre_age(y, now) < 0) snprintf(buf, sizeof(buf), v->on ? "waiting\xE2\x80\xA6" : "off");
    else snprintf(buf, sizeof(buf), "no signal");
    ui_label_set(u->temp, buf);
    bool hot = v->cfg.hot_on && !isnan(y->temp_c) && y->temp_c > v->cfg.hot_c;
    ui_label_color(u->temp, !ok ? C_DIM : hot ? C_ALERT : C_TEXT);
    update_battery(u, y, ok);
}

static void update(const dash_t *d, float dt, bool visible)
{
    if (d->now - s_last < UPDATE_MS) return;
    s_last = d->now;
    s_v = d->tpms;
    s_now = d->now;
    update_pairing(&s_v);
    if (!visible) return;
    for (int t = 0; t < TPMS_TYRES; t++) update_tyre(&s_v, t, d->now);
    bool any = s_v.pair[0].format || s_v.pair[1].format;
    const char *hint = !s_v.on && !s_v.demo ? "Tyre sensors are off (Status)" : any ? "" : "Hold to pair the sensors";
    ui_label_set(s_hint, hint);
    if (hint[0]) {
        lv_obj_add_flag(s_cap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_cap, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

const ui_page_t PAGE_TYRES = {create, update, hold};
