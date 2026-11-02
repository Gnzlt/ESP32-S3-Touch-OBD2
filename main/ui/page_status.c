/*
 * Page 8, Status and settings: buttons that open Rides and Diagnostics, the
 * link (adapter, protocol, ECU, replies a second, errors), the board's battery,
 * and the settings: brightness, beeps, alerts, tyre sensors and their pressure
 * unit, demo mode, battery saver, forget the adapter. Scrolls vertically.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ui_common.h"
#include "bike/bike.h"
#include "sd_log.h"

typedef enum { R_ADAPTER, R_ELM, R_PROTO, R_ECU, R_ECU_SW, R_RATE, R_ERRORS, R_BATTERY, R_TEMP, R_SD, R_TYRES, R_COUNT } row_id_t;

static lv_obj_t *s_col, *s_row[R_COUNT];
static lv_obj_t *s_sw_beeps, *s_sw_alerts, *s_sw_demo, *s_sw_saver, *s_sw_sd, *s_sw_tpms, *s_slider, *s_unit;
static lv_obj_t *s_rides_sub, *s_diag_sub;
static uint32_t s_formats;
static bool s_format_asked;
static int64_t s_last;

static lv_obj_t *row(const char *caption, row_id_t id)
{
    lv_obj_t *r = ui_box(s_col, 250, LV_SIZE_CONTENT);
    lv_obj_t *c = ui_label(r, &font_txt_14, C_DIM, caption);
    lv_obj_align(c, LV_ALIGN_LEFT_MID, 0, 0);
    s_row[id] = ui_label(r, &font_txt_14, C_TEXT, "--");
    lv_obj_set_width(s_row[id], 160);
    lv_obj_set_style_text_align(s_row[id], LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_row[id], LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(s_row[id], LV_ALIGN_RIGHT_MID, 0, 0);
    return r;
}

static void heading(const char *text)
{
    lv_obj_t *h = ui_label(s_col, &font_txt_14, C_ACCENT, text);
    lv_obj_set_style_text_letter_space(h, 3, 0);
    lv_obj_set_style_pad_top(h, 10, 0);
}

static void on_switch(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    settings_t *st = app_settings();
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (sw == s_sw_beeps) st->beeps = on;
    if (sw == s_sw_alerts) st->alerts = on;
    if (sw == s_sw_demo) st->demo = on;
    if (sw == s_sw_saver) st->saver = on;
    if (sw == s_sw_sd) st->sd_log = on;
    if (sw == s_sw_tpms) st->tpms = on;
    app_settings_changed();
    ui_tick_feedback();
}

static void on_slider(lv_event_t *e)
{
    app_settings()->brightness = lv_slider_get_value(s_slider);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        app_settings_changed();
    } else {
        app_request(REQ_SETTINGS_CHANGED);   // live preview, saved on release
    }
}

// Tap to cycle bar -> psi -> kPa.
static void on_unit(lv_event_t *e)
{
    settings_t *st = app_settings();
    st->pressure_unit = (uint8_t)((st->pressure_unit + 1) % TPMS_UNIT_COUNT);
    ui_label_set(s_unit, tpms_unit_name((tpms_unit_t)st->pressure_unit));
    app_settings_changed();
    ui_tick_feedback();
}

static void unit_row(void)
{
    lv_obj_t *r = ui_box(s_col, 250, 36);
    lv_obj_t *c = ui_label(r, &font_txt_18, C_TEXT, "Pressure unit");
    lv_obj_align(c, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *btn = lv_button_create(r);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 70, 32);
    lv_obj_set_style_radius(btn, 16, 0);
    lv_obj_set_style_bg_color(btn, C_TRACK_HI, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(btn, C_FAINT, LV_STATE_PRESSED);
    lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(btn, on_unit, LV_EVENT_CLICKED, NULL);
    s_unit = ui_label(btn, &font_txt_18, C_ACCENT, tpms_unit_name((tpms_unit_t)app_settings()->pressure_unit));
    lv_obj_center(s_unit);
}

static void forget(void)
{
    app_request(REQ_FORGET_ADAPTER);
    ui_toast("Adapter forgotten");
}

static void on_forget(lv_event_t *e)
{
    ui_tick_feedback();
    ui_confirm("Forget the adapter?", "The next connection scans\nand searches the protocol again.", "Forget",
               C_WARN, forget);
}

static void format_card(void)
{
    s_format_asked = true;
    sd_log_format();
    ui_toast("Formatting the card...");
}

static void on_format(lv_event_t *e)
{
    ui_tick_feedback();
    ui_confirm("Format SD card?", "Erases everything on it,\nride logs included.", "Format", C_ALERT,
               format_card);
}

static lv_obj_t *outline_button(const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(s_col);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 200, 40);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, C_FAINT, 0);
    lv_obj_set_style_bg_color(btn, C_TRACK_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_margin_top(btn, 8, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bt = ui_label(btn, &font_txt_18, C_DIM, text);
    lv_obj_center(bt);
    return btn;
}

static void on_open(lv_event_t *e)
{
    ui_tick_feedback();
    ui_view_open(lv_event_get_user_data(e));
}

// A tile that opens a view, with a live line under its title.
static lv_obj_t *view_tile(lv_obj_t *parent, const char *title, const ui_page_t *view)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 121, 58);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, C_TRACK_HI, 0);
    lv_obj_set_style_bg_color(btn, C_TRACK_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, on_open, LV_EVENT_CLICKED, (void *)view);
    lv_obj_t *t = ui_label(btn, &font_txt_18, C_TEXT, title);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 8);
    lv_obj_t *sub = ui_label(btn, &font_txt_14, C_DIM, "--");
    lv_obj_align(sub, LV_ALIGN_BOTTOM_MID, 0, -8);
    return sub;
}

static lv_obj_t *switch_row(const char *caption, bool on)
{
    lv_obj_t *r = ui_box(s_col, 250, 36);
    lv_obj_t *c = ui_label(r, &font_txt_18, C_TEXT, caption);
    lv_obj_align(c, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 54, 30);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(sw, C_TRACK_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, C_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    lv_obj_add_event_cb(sw, on_switch, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static void create(lv_obj_t *tile)
{
    s_col = ui_column(tile);

    lv_obj_t *tiles = ui_box(s_col, 250, 58);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tiles, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_rides_sub = view_tile(tiles, "Rides", &PAGE_RIDES);
    s_diag_sub = view_tile(tiles, "Diagnostics", &PAGE_DIAG);

    heading("LINK");
    row("Adapter", R_ADAPTER);
    row("Chip", R_ELM);
    row("Protocol", R_PROTO);
    row("ECU", R_ECU);
    row("ECU sw", R_ECU_SW);   // Mode 09 calibration ID (the VIN stays off the screen)
    row("Replies/s", R_RATE);
    row("Errors", R_ERRORS);
    heading("DEVICE");
    row("Battery", R_BATTERY);
    row("Temp", R_TEMP);
    row("SD card", R_SD);
    row("Tyres", R_TYRES);

    heading("SETTINGS");
    lv_obj_t *bl = ui_label(s_col, &font_txt_18, C_TEXT, "Brightness");
    (void)bl;
    s_slider = lv_slider_create(s_col);
    lv_obj_set_width(s_slider, 220);
    lv_slider_set_range(s_slider, 10, 100);
    lv_slider_set_value(s_slider, app_settings()->brightness, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_slider, C_TRACK_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider, C_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider, C_TEXT, LV_PART_KNOB);
    lv_obj_set_style_margin_ver(s_slider, 10, 0);
    lv_obj_add_event_cb(s_slider, on_slider, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_slider, on_slider, LV_EVENT_RELEASED, NULL);
    s_sw_beeps = switch_row("Alert beeps", app_settings()->beeps);
    s_sw_alerts = switch_row("Alerts", app_settings()->alerts);
    s_sw_sd = switch_row("SD logging", app_settings()->sd_log);
    s_sw_tpms = switch_row("Tyre sensors", app_settings()->tpms);
    unit_row();
    s_sw_demo = switch_row("Demo mode", app_settings()->demo);
    s_sw_saver = switch_row("Battery saver", app_settings()->saver);
    outline_button("Forget adapter", on_forget);
    outline_button("Format SD card", on_format);

    lv_obj_t *ver = ui_label(s_col, &font_txt_14, C_FAINT, "OBD2 dashboard  \xE2\x80\x93  " BIKE_NAME);
    lv_obj_set_style_pad_top(ver, 12, 0);
    ui_label(s_col, &font_txt_14, C_DIM, "Made by Gnzlt.com");
}

static const char *phase_text(obd_phase_t p)
{
    switch (p) {
    case PHASE_NO_BLE: return "Bluetooth failed";
    case PHASE_SCAN: return "Searching";
    case PHASE_CONNECT: return "Connecting";
    case PHASE_INIT: return "Starting session";
    case PHASE_ECU_OFF: return "Ignition off?";
    case PHASE_LIVE: return "Live";
    }
    return "";
}

static void update(const dash_t *d, float dt, bool visible)
{
    if (!visible || d->now - s_last < 500) {
        return;
    }
    s_last = d->now;
    const obd_state_t *o = &d->obd;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s", o->ble_name[0] ? o->ble_name : phase_text(o->phase));
    ui_label_set(s_row[R_ADAPTER], buf);
    ui_label_set(s_row[R_ELM], o->adapter[0] ? o->adapter : "--");
    const char *proto = o->protocol[0] == '7' ? "7  CAN 29-bit 500k" : o->protocol[0] == '6' ? "6  CAN 11-bit 500k"
                      : o->protocol[0] ? o->protocol : "--";
    snprintf(buf, sizeof(buf), "%s%s", proto, o->multi_pid ? "  multi" : "");
    ui_label_set(s_row[R_PROTO], buf);
    ui_label_set(s_row[R_ECU], o->ecu[0] ? o->ecu : "--");
    ui_label_set(s_row[R_ECU_SW], o->cal_id[0][0] ? o->cal_id[0] : o->info_read ? "not reported" : "--");
    snprintf(buf, sizeof(buf), "%.1f", o->replies_per_sec);
    ui_label_set(s_row[R_RATE], o->phase == PHASE_LIVE ? buf : "--");
    snprintf(buf, sizeof(buf), "%lu to  %lu re  %lu dr%s%s", (unsigned long)o->timeouts, (unsigned long)o->reinits,
             (unsigned long)o->link_drops, o->last_error[0] ? "  " : "", o->last_error);
    ui_label_set(s_row[R_ERRORS], buf);
    if (d->battery_ok && d->battery.present) {
        // The current too (+ charging, - draw), to compare normal and saver.
        snprintf(buf, sizeof(buf), "%u%%  %.2f V  %+dmA", d->battery.soc_percent, d->battery.voltage_mv / 1000.0f,
                 d->battery.current_ma);
    } else {
        snprintf(buf, sizeof(buf), d->battery_ok ? "USB power" : "--");
    }
    ui_label_set(s_row[R_BATTERY], buf);
    // The chip and, with a cell, the gauge beside it: amber from the warning
    // level, red from the critical one (obd/alerts.h).
    bool cell = d->battery_ok && d->battery.present;
    int n = 0;
    buf[0] = 0;
    if (!isnan(d->chip_c)) n += snprintf(buf, sizeof(buf), "chip %d\xC2\xB0", (int)roundf(d->chip_c));
    if (cell) snprintf(buf + n, sizeof(buf) - n, "%sbatt %d\xC2\xB0", n ? "  " : "", (int)roundf(d->battery.temp_c));
    ui_label_set(s_row[R_TEMP], buf[0] ? buf : "--");
    bool crit = (!isnan(d->chip_c) && d->chip_c >= CHIP_CRIT_C) || (cell && (d->battery.temp_c >= CELL_CRIT_C || d->battery.overtemp));
    bool warn = (!isnan(d->chip_c) && d->chip_c >= CHIP_WARN_C) || (cell && d->battery.temp_c >= CELL_WARN_C);
    ui_label_color(s_row[R_TEMP], crit ? C_ALERT : warn ? C_WARN : C_TEXT);
    sd_status_t sd;
    sd_log_status(&sd);
    if (sd.formatting) {
        snprintf(buf, sizeof(buf), "formatting...");
    } else if (!sd.mounted) {
        snprintf(buf, sizeof(buf), "%s", sd.error[0] ? sd.error : "no card");
    } else if (sd.logging) {
        snprintf(buf, sizeof(buf), "ride %d  %lu KB  %.1f GB free", sd.ride, (unsigned long)sd.written_kb,
                 sd.free_mb / 1024.0f);
    } else {
        snprintf(buf, sizeof(buf), "%.1f GB free", sd.free_mb / 1024.0f);
    }
    ui_label_set(s_row[R_SD], buf);
    if (sd.logging) snprintf(buf, sizeof(buf), "ride %d now", sd.ride);
    ui_label_set(s_rides_sub, sd.logging ? buf : sd.mounted ? "on the card" : "no card");
    ui_label_color(s_rides_sub, sd.logging ? C_OK : C_DIM);
    bool live = o->phase == PHASE_LIVE;
    int codes = o->n_dtcs;   // stored, plus the pending ones not stored too (as Diagnostics lists them)
    for (int i = 0; i < o->n_pending; i++) {
        bool dup = false;
        for (int k = 0; k < o->n_dtcs; k++) dup |= strcmp(o->dtcs[k], o->pending[i]) == 0;
        codes += !dup;
    }
    snprintf(buf, sizeof(buf), "%d code%s", codes, codes == 1 ? "" : "s");
    ui_label_set(s_diag_sub, !live ? "--" : codes ? buf : o->mil ? "MIL ON" : o->dtcs_read ? "no codes" : "--");
    ui_label_color(s_diag_sub, !live ? C_DIM : o->n_dtcs ? C_ALERT : codes || o->mil ? C_WARN : C_DIM);
    const tpms_view_t *tp = &d->tpms;
    if (!tp->on) {
        snprintf(buf, sizeof(buf), "off");
    } else {
        int n = 0;
        buf[0] = 0;
        for (int t = 0; t < TPMS_TYRES; t++) {
            char p[12] = "--";
            if (tp->pair[t].format && !isnan(tp->tyre[t].kpa) && tpms_tyre_age(&tp->tyre[t], d->now) < TPMS_STALE_MS) {
                tpms_fmt_pressure(p, sizeof(p), tp->tyre[t].kpa, (tpms_unit_t)app_settings()->pressure_unit);
            } else if (!tp->pair[t].format) {
                snprintf(p, sizeof(p), "unpaired");
            }
            n += snprintf(buf + n, sizeof(buf) - n, "%s%c %s", t ? "  " : "", t ? 'R' : 'F', p);
        }
    }
    ui_label_set(s_row[R_TYRES], buf);
    if (s_format_asked && !sd.formatting && (sd.formats != s_formats || sd.error[0])) {
        s_format_asked = false;
        ui_toast(sd.formats != s_formats ? "Card formatted" : "Format failed");
    }
    s_formats = sd.formats;
    // Keep the switches in step if demo mode or battery saver changed elsewhere
    // (saver turns itself on at 20 %).
    if (lv_obj_has_state(s_sw_demo, LV_STATE_CHECKED) != app_settings()->demo) {
        if (app_settings()->demo) lv_obj_add_state(s_sw_demo, LV_STATE_CHECKED);
        else lv_obj_remove_state(s_sw_demo, LV_STATE_CHECKED);
    }
    if (lv_obj_has_state(s_sw_saver, LV_STATE_CHECKED) != (bool)app_settings()->saver) {
        if (app_settings()->saver) lv_obj_add_state(s_sw_saver, LV_STATE_CHECKED);
        else lv_obj_remove_state(s_sw_saver, LV_STATE_CHECKED);
    }
}

const ui_page_t PAGE_STATUS = {create, update, NULL};
