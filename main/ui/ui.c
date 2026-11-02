/*
 * The shell around the pages: the boot splash, the swipeable pager with its
 * page segments and the BOOT button, the views opened from Status (Rides,
 * Diagnostics), the board battery icon, the connection pill, the alert
 * overlay, toasts, the confirm dialog, sheets, the "parked" screen and the
 * battery saver screen.
 * One LVGL timer (~30 Hz) copies the shared snapshot and updates every page, so
 * LVGL is only touched from its own task. In battery saver it ticks once a
 * second and the pages aren't touched at all.
 */
#include "bsp/esp-bsp.h"   /* before math.h: the IMU header in it defines M_PI itself */
#include <math.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include "bsp/display.h"
#include "button_gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "iot_button.h"
#include "mem_report.h"
#include "sdkconfig.h"
#include "sensors/beeper.h"
#include "ui.h"
#include "ui_common.h"
#include "bike/bike.h"
#include "obd/board_guard.h"

LV_IMAGE_DECLARE(splash_bike);

#define FRAME_MS        33
#define SPLASH_MIN_MS   2200
#define PARKED_AFTER_MS 120000
#define PILL_SHOW_MS    2500
#define ALERT_SHOW_MS   6000
#define SAVER_BRIGHTNESS 15      // %, or the slider's level if that is lower
#define SAVER_TICK_MS   1000     // the frame timer in saver: buttons and alerts
#define SAVER_UPDATE_MS 30000    // how often the saver screen's values change
#define PEEK_MS         30000    // untouched this long, a peek goes back to saver

static const ui_page_t *PAGES[] = {&PAGE_RIDE, &PAGE_ENGINE, &PAGE_TYRES, &PAGE_PERF, &PAGE_LEAN, &PAGE_TRIP,
                                     &PAGE_FUEL, &PAGE_STATUS};
#define N_PAGES ((int)(sizeof(PAGES) / sizeof(PAGES[0])))
// Opened from the Status page, over the pager (ui_view_open).
static const ui_page_t *VIEWS[] = {&PAGE_RIDES, &PAGE_DIAG};
#define N_VIEWS ((int)(sizeof(VIEWS) / sizeof(VIEWS[0])))

static lv_obj_t *s_main, *s_tv, *s_segs[N_PAGES], *s_views[N_VIEWS];
static int s_view = -1;   // the open view, -1 for none
static lv_obj_t *s_pill, *s_pill_dot, *s_pill_text;
static lv_obj_t *s_alert, *s_alert_icon, *s_alert_title, *s_alert_detail;
static lv_obj_t *s_toast, *s_toast_text;
static lv_obj_t *s_modal;
static lv_obj_t *s_parked, *s_parked_batt;
static lv_obj_t *s_batt, *s_batt_body, *s_batt_nub, *s_batt_fill;
static lv_obj_t *s_saver, *s_sv_state, *s_sv_time, *s_sv_dist, *s_sv_tyres, *s_sv_alert, *s_sv_batt;
static lv_timer_t *s_frame;
static void (*s_confirm_ok)(void);

static EXT_RAM_BSS_ATTR dash_t s_dash;
static uint32_t s_last_alert_seq;
static uint32_t s_last_frame;
static int64_t s_alert_until, s_pill_until, s_not_live_since;
static obd_phase_t s_last_phase = (obd_phase_t)-1;
static int s_last_page = -1;
static bool s_last_demo;
static bool s_parked_shown;
static bool s_off_shown;             // the board is switching itself off (board_guard.h)
static uint32_t s_refreshes, s_fps_from;
static bool s_saver_on;              // the saver screen is loaded
static bool s_peek;                  // saver is set, but the pages show until PEEK_MS untouched
static int64_t s_saver_drawn;        // when its values last changed
static int64_t s_saver_bright_until; // an alert lights the saver screen up until then

static void log_fps(void);

// The backlight has one owner, here: set only when the wanted level changes.
static int s_brightness = -1;

static void set_brightness(int v)
{
    if (v != s_brightness) {
        s_brightness = v;
        bsp_display_brightness_set(v);
    }
}

// ---- Small animation helpers ------------------------------------------------

static void anim_opa_cb(void *obj, int32_t v) { lv_obj_set_style_opa(obj, (lv_opa_t)v, 0); }
static void anim_bg_opa_cb(void *obj, int32_t v) { lv_obj_set_style_bg_opa(obj, (lv_opa_t)v, 0); }
static void anim_brightness_cb(void *obj, int32_t v) { bsp_display_brightness_set(v); }
static void anim_letter_cb(void *obj, int32_t v) { lv_obj_set_style_text_letter_space(obj, v, 0); }
static void anim_arc_cb(void *obj, int32_t v) { lv_arc_set_angles(obj, 270, 270 + v); }
static void anim_y_cb(void *obj, int32_t v) { lv_obj_set_style_translate_y(obj, v, 0); }

static void fade(lv_obj_t *obj, int from, int to, int ms, int delay)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_delay(&a, delay);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void hide_cb(lv_anim_t *a) { lv_obj_add_flag(a->var, LV_OBJ_FLAG_HIDDEN); }

static void fade_out_hide(lv_obj_t *obj, int ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, lv_obj_get_style_opa(obj, 0), 0);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_completed_cb(&a, hide_cb);
    lv_anim_start(&a);
}

static void show_fade_in(lv_obj_t *obj, int ms)
{
    lv_anim_delete(obj, anim_opa_cb);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    fade(obj, lv_obj_get_style_opa(obj, 0), LV_OPA_COVER, ms, 0);
}

// ---- Page indicator -------------------------------------------------------------
// A split gauge on the bottom rim, in the gap every page's dial leaves: one
// short segment per page, page 1 on the left. The current one glows gold, and
// the glow slides between segments as the pager scrolls.

#define SEG_DEG 6
#define GAP_DEG 3
#define SEG_LEFT (90 + (N_PAGES * SEG_DEG + (N_PAGES - 1) * GAP_DEG) / 2)

static void update_segments(void)
{
    float pos = lv_obj_get_scroll_x(s_tv) / (float)UI_W;
    for (int i = 0; i < N_PAGES; i++) {
        float k = 1.0f - fminf(1.0f, fabsf(pos - i));
        lv_color_t c = lv_color_mix(C_ACCENT, C_FAINT, (uint8_t)(255 * k));
        if (!lv_color_eq(lv_obj_get_style_arc_color(s_segs[i], LV_PART_MAIN), c)) {
            lv_obj_set_style_arc_color(s_segs[i], c, LV_PART_MAIN);
        }
    }
}

// ---- Board battery ------------------------------------------------------------
// A small battery icon at the bottom centre, above the page segments: the fill
// follows the charge, green, amber at 20 %, red under 10 %, breathing cyan while
// charging. Hidden without a cell. A tap shows the details in a toast.

#define BATT_FILL_W 18

static bool batt_present(const dash_t *d)
{
    return d->battery_ok && d->battery.present;
}

static lv_color_t batt_color(const battery_info_t *b)
{
    return b->charging ? C_CYAN : b->soc_percent < 10 ? C_ALERT : b->soc_percent <= 20 ? C_WARN : C_OK;
}

static void on_batt_tap(lv_event_t *e)
{
    ui_tick_feedback();
    const battery_info_t *b = &s_dash.battery;
    char buf[64];
    if (!batt_present(&s_dash)) {
        snprintf(buf, sizeof(buf), s_dash.battery_ok ? "USB power, no battery" : "Battery gauge not read");
    } else if (b->charging) {
        snprintf(buf, sizeof(buf), "Board %u%%  \xE2\x80\x93  charging", b->soc_percent);
    } else if (b->time_to_empty != 0xFFFF && b->time_to_empty > 0) {
        snprintf(buf, sizeof(buf), "Board %u%%  \xE2\x80\x93  %.2f V\n~%dh %02dm left", b->soc_percent,
                 b->voltage_mv / 1000.0f, b->time_to_empty / 60, b->time_to_empty % 60);
    } else {
        snprintf(buf, sizeof(buf), "Board %u%%  \xE2\x80\x93  %.2f V", b->soc_percent, b->voltage_mv / 1000.0f);
    }
    ui_toast(buf);
}

static void update_battery(const dash_t *d)
{
    if (!batt_present(d)) {
        if (!lv_obj_has_flag(s_batt, LV_OBJ_FLAG_HIDDEN)) lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (lv_obj_has_flag(s_batt, LV_OBJ_FLAG_HIDDEN)) lv_obj_remove_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
    const battery_info_t *b = &d->battery;
    // A sliver stays visible at 0 % so the icon still reads as a battery level.
    int soc = b->soc_percent > 100 ? 100 : b->soc_percent;
    int w = soc * BATT_FILL_W / 100;
    if (w < 2) w = 2;
    if (lv_obj_get_width(s_batt_fill) != w) {
        lv_obj_set_width(s_batt_fill, w);
    }
    lv_color_t c = batt_color(b);
    if (!lv_color_eq(lv_obj_get_style_bg_color(s_batt_fill, 0), c)) {
        lv_obj_set_style_bg_color(s_batt_fill, c, 0);
    }
    // The outline turns the fill's colour once the level needs attention.
    lv_color_t edge = b->charging || soc <= 20 ? c : C_DIM;
    if (!lv_color_eq(lv_obj_get_style_border_color(s_batt_body, 0), edge)) {
        lv_obj_set_style_border_color(s_batt_body, edge, 0);
        lv_obj_set_style_bg_color(s_batt_nub, edge, 0);
    }
    lv_opa_t opa = b->charging ? (lv_opa_t)(140 + 115 * sinf(d->now / 500.0f)) : LV_OPA_COVER;
    if (lv_obj_get_style_bg_opa(s_batt_fill, 0) != opa) {
        lv_obj_set_style_bg_opa(s_batt_fill, opa, 0);
    }
}

static void update_parked_battery(const dash_t *d)
{
    char buf[40];
    const battery_info_t *b = &d->battery;
    if (!batt_present(d)) {
        ui_label_set(s_parked_batt, "");
        return;
    }
    snprintf(buf, sizeof(buf), "Board %u%%%s", b->soc_percent, b->charging ? "  \xE2\x80\x93  charging" : "");
    ui_label_set(s_parked_batt, buf);
    ui_label_color(s_parked_batt, b->charging ? C_CYAN : b->soc_percent <= 20 ? C_WARN : C_DIM);
}

// ---- Connection pill ----------------------------------------------------------

static void pill_show(const char *text, lv_color_t color, bool sticky, int64_t now)
{
    ui_label_set(s_pill_text, text);
    ui_label_color(s_pill_text, color);
    lv_obj_set_style_bg_color(s_pill_dot, color, 0);
    s_pill_until = sticky ? INT64_MAX : now + PILL_SHOW_MS;
    if (lv_obj_has_flag(s_pill, LV_OBJ_FLAG_HIDDEN) || lv_obj_get_style_opa(s_pill, 0) < LV_OPA_COVER) {
        show_fade_in(s_pill, 250);
    }
}

static void update_pill(const dash_t *d, int page)
{
    obd_phase_t ph = d->obd.phase;
    bool changed = ph != s_last_phase || d->demo != s_last_demo;
    if (changed) {
        s_last_phase = ph;
        s_last_demo = d->demo;
        if (d->demo) {
            pill_show("DEMO", C_ACCENT, false, d->now);
        } else {
            switch (ph) {
            case PHASE_NO_BLE: pill_show("NO BLUETOOTH", C_ALERT, true, d->now); break;
            case PHASE_SCAN: pill_show("SEARCHING", C_DIM, true, d->now); break;
            case PHASE_CONNECT: pill_show("CONNECTING", C_DIM, true, d->now); break;
            case PHASE_INIT: pill_show("STARTING", C_DIM, true, d->now); break;
            case PHASE_ECU_OFF: pill_show("IGNITION OFF", C_WARN, true, d->now); break;
            case PHASE_LIVE: pill_show("CONNECTED", C_OK, false, d->now); break;
            }
        }
    } else if (page != s_last_page && d->demo) {
        pill_show("DEMO", C_ACCENT, false, d->now);
    }
    if (s_pill_until != INT64_MAX && d->now > s_pill_until && !lv_obj_has_flag(s_pill, LV_OBJ_FLAG_HIDDEN)
        && !lv_anim_get(s_pill, anim_opa_cb)) {
        fade_out_hide(s_pill, 400);
    }
    // While searching, the dot breathes.
    if (s_pill_until == INT64_MAX && ph != PHASE_ECU_OFF && ph != PHASE_NO_BLE) {
        lv_obj_set_style_bg_opa(s_pill_dot, (lv_opa_t)(140 + 115 * sinf(d->now / 300.0f)), 0);
    } else {
        lv_obj_set_style_bg_opa(s_pill_dot, LV_OPA_COVER, 0);
    }
}

// ---- Alert overlay ------------------------------------------------------------

static void alert_dismiss(lv_event_t *e)
{
    s_alert_until = 0;
}

// The words for the latest alert: a title, a detail (may be empty) and its
// colour. False for none. The overlay and the saver screen both use it.
static bool alert_text(const dash_t *d, char *title, size_t tn, char *detail, size_t dn, lv_color_t *color)
{
    float v;
    detail[0] = 0;
    *color = C_WARN;
    switch (d->alert) {
    case ALERT_COOLANT:
        *color = C_ALERT;
        snprintf(title, tn, "ENGINE HOT");
        if (obd_value(&d->obd, PID_COOLANT, d->now, &v)) snprintf(detail, dn, "%d\xC2\xB0", (int)roundf(v));
        break;
    case ALERT_VOLT_LOW:
        snprintf(title, tn, "NOT CHARGING");
        if (obd_value(&d->obd, PID_VOLTAGE, d->now, &v)) snprintf(detail, dn, "%.1f V", v);
        break;
    case ALERT_VOLT_HIGH:
        snprintf(title, tn, "VOLTAGE HIGH");
        if (obd_value(&d->obd, PID_VOLTAGE, d->now, &v)) snprintf(detail, dn, "%.1f V", v);
        break;
    case ALERT_BOARD_BATT:
        snprintf(title, tn, "BOARD BATTERY");
        *color = d->battery.soc_percent <= 10 ? C_ALERT : C_WARN;
        snprintf(detail, dn, "%u%%", d->battery.soc_percent);
        break;
    case ALERT_BOARD_HOT:
        snprintf(title, tn, "%s", d->hot_source == HOT_CELL ? "BATTERY HOT" : "BOARD HOT");
        *color = d->hot_critical ? C_ALERT : C_WARN;
        if (!isnan(d->hot_c)) snprintf(detail, dn, "%d\xC2\xB0", (int)roundf(d->hot_c));
        else snprintf(detail, dn, "over temperature");
        break;
    case ALERT_DTC:
        snprintf(title, tn, "FAULT CODE");
        if (d->obd.n_dtcs > 0) snprintf(detail, dn, "%s", d->obd.dtcs[0]);
        else if (d->obd.n_pending > 0) snprintf(detail, dn, "%s", d->obd.pending[0]);
        else snprintf(detail, dn, "MIL ON");
        break;
    case ALERT_TYRE_LOSS:
    case ALERT_TYRE_LOW:
    case ALERT_TYRE_ALARM:
    case ALERT_TYRE_HOT:
    case ALERT_TYRE_HIGH:
    case ALERT_TYRE_COLD:
    case ALERT_TYRE_SILENT:
    case ALERT_TYRE_BATT: {
        static const char *WHAT[] = {"LOSING AIR", "LOW", "SENSOR ALARM", "HOT", "HIGH", "COLD", "NO SIGNAL",
                                     "SENSOR BATTERY"};
        int k = d->alert - ALERT_TYRE_LOSS, tyre = d->alert_tyre == TPMS_REAR ? TPMS_REAR : TPMS_FRONT;
        snprintf(title, tn, "%s %s", TPMS_TYRE_CAPS[tyre], WHAT[k]);
        *color = d->alert <= ALERT_TYRE_HOT ? C_ALERT : C_WARN;
        const tpms_tyre_t *y = &d->tpms.tyre[tyre];
        tpms_unit_t u = (tpms_unit_t)app_settings()->pressure_unit;
        char p[12];
        if (d->alert == ALERT_TYRE_HOT || d->alert == ALERT_TYRE_COLD) {
            if (!isnan(y->temp_c)) snprintf(detail, dn, "%d\xC2\xB0", (int)roundf(y->temp_c));
        } else if (d->alert == ALERT_TYRE_BATT) {
            int lvl = tpms_battery_level(y->battery_pct, y->volts);
            if (lvl >= 0) snprintf(detail, dn, "%d%%", lvl);
        } else if (d->alert != ALERT_TYRE_SILENT && !isnan(y->kpa)) {
            tpms_fmt_pressure(p, sizeof(p), y->kpa, u);
            snprintf(detail, dn, "%s %s", p, tpms_unit_name(u));
        }
        break;
    }
    default:
        return false;
    }
    return true;
}

static void alert_show(const dash_t *d)
{
    char title[40], detail[48];
    lv_color_t color;
    if (!alert_text(d, title, sizeof(title), detail, sizeof(detail), &color)) {
        return;
    }
    lv_obj_set_style_bg_color(s_alert, color, 0);
    ui_label_color(s_alert_icon, color);
    ui_label_color(s_alert_title, color);
    ui_label_set(s_alert_title, title);
    ui_label_set(s_alert_detail, detail);
    lv_obj_move_foreground(s_alert);
    lv_obj_remove_flag(s_alert, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(s_alert, LV_OPA_COVER, 0);
    // The red wash pulses: 3 slow breaths, then it holds.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_alert);
    lv_anim_set_exec_cb(&a, anim_bg_opa_cb);
    lv_anim_set_values(&a, 40, 110);
    lv_anim_set_duration(&a, 450);
    lv_anim_set_reverse_duration(&a, 450);
    lv_anim_set_repeat_count(&a, 3);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
    s_alert_until = d->now + ALERT_SHOW_MS;
}

static lv_obj_t *build_alert(void)
{
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, UI_W, UI_H);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(o, 60, 0);
    lv_obj_set_style_border_width(o, 6, 0);
    lv_obj_set_style_border_color(o, C_ALERT, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, alert_dismiss, LV_EVENT_CLICKED, NULL);
    // A dark disc behind the text keeps it readable over the wash.
    lv_obj_t *disc = ui_box(o, 250, 250);
    lv_obj_center(disc);
    lv_obj_set_style_radius(disc, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(disc, C_BG, 0);
    lv_obj_set_style_bg_opa(disc, LV_OPA_80, 0);
    s_alert_icon = ui_label(o, &lv_font_montserrat_48, C_ALERT, LV_SYMBOL_WARNING);
    lv_obj_align(s_alert_icon, LV_ALIGN_CENTER, 0, -68);
    s_alert_title = ui_label(o, &font_txt_24, C_ALERT, "");
    lv_obj_set_style_text_letter_space(s_alert_title, 2, 0);
    lv_obj_align(s_alert_title, LV_ALIGN_CENTER, 0, -14);
    s_alert_detail = ui_label(o, &font_num_48, C_TEXT, "");
    lv_obj_align(s_alert_detail, LV_ALIGN_CENTER, 0, 36);
    lv_obj_t *hint = ui_label(o, &font_txt_14, C_DIM, "tap to dismiss");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 84);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

// ---- Toast and confirm ----------------------------------------------------------

void ui_toast(const char *text)
{
    lv_label_set_text(s_toast_text, text);
    lv_obj_move_foreground(s_toast);
    lv_anim_delete(s_toast, NULL);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    fade(s_toast, 0, LV_OPA_COVER, 200, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, 12, 0);
    lv_anim_set_duration(&a, 250);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, 0);
    lv_anim_set_duration(&a, 350);
    lv_anim_set_delay(&a, 1600);
    lv_anim_set_completed_cb(&a, hide_cb);
    lv_anim_start(&a);
}

void ui_tick_feedback(void)
{
    if (app_settings()->beeps) {
        beeper_play(BEEP_TICK);
    }
}

static void modal_close(void)
{
    lv_obj_remove_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
    fade_out_hide(s_modal, 180);
}

static void modal_cancel(lv_event_t *e) { modal_close(); }

static void modal_ok(lv_event_t *e)
{
    modal_close();
    if (s_confirm_ok) s_confirm_ok();
}

static lv_obj_t *modal_button(lv_obj_t *parent, const char *text, lv_color_t color, int x, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, 108, 44);
    lv_obj_align(b, LV_ALIGN_CENTER, x, 74);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_border_width(b, 2, 0);
    lv_obj_set_style_border_color(b, color, 0);
    lv_obj_set_style_bg_color(b, color, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(b, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = ui_label(b, &font_txt_18, color, text);
    lv_obj_center(l);
    return b;
}

void ui_confirm(const char *title, const char *body, const char *ok_text, lv_color_t ok_color, void (*on_ok)(void))
{
    if (s_modal) {
        lv_obj_delete(s_modal);
    }
    s_confirm_ok = on_ok;
    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_modal, C_BG, 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_90, 0);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);   // swallows touches behind it
    lv_obj_t *t = ui_label(s_modal, &font_txt_24, C_TEXT, title);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -62);
    lv_obj_t *b = ui_label(s_modal, &font_txt_14, C_DIM, body);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, -6);
    modal_button(s_modal, "Cancel", C_DIM, -60, modal_cancel);
    modal_button(s_modal, ok_text, ok_color, 60, modal_ok);
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
    fade(s_modal, 0, LV_OPA_COVER, 180, 0);
}

// ---- Number picker ------------------------------------------------------------

static lv_obj_t *s_pick_whole, *s_pick_tenth;
static void (*s_pick_ok)(float);

static void picker_ok(lv_event_t *e)
{
    float v = lv_roller_get_selected(s_pick_whole) + (s_pick_tenth ? lv_roller_get_selected(s_pick_tenth) / 10.0f : 0);
    modal_close();
    if (s_pick_ok) s_pick_ok(v);
}

static lv_obj_t *picker_roller(const char *options, int sel, int x, int w)
{
    lv_obj_t *r = lv_roller_create(s_modal);
    lv_roller_set_options(r, options, LV_ROLLER_MODE_NORMAL);
    lv_roller_set_visible_row_count(r, 3);
    lv_roller_set_selected(r, sel, LV_ANIM_OFF);
    lv_obj_set_width(r, w);
    lv_obj_align(r, LV_ALIGN_CENTER, x, 2);
    lv_obj_set_style_text_font(r, &font_txt_24, 0);
    lv_obj_set_style_text_color(r, C_DIM, 0);
    lv_obj_set_style_bg_opa(r, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(r, 0, 0);
    lv_obj_set_style_text_font(r, &font_txt_24, LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, C_TEXT, LV_PART_SELECTED);
    lv_obj_set_style_bg_color(r, C_TRACK_HI, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_radius(r, 8, LV_PART_SELECTED);
    return r;
}

void ui_number_picker(const char *title, const char *body, int max_whole, bool tenths, const char *unit, float initial,
                      void (*on_ok)(float))
{
    if (s_modal) {
        lv_obj_delete(s_modal);
    }
    s_pick_ok = on_ok;
    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_modal, C_BG, 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);   // swallows touches behind it
    lv_obj_t *t = ui_label(s_modal, &font_txt_24, C_TEXT, title);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, -112);
    lv_obj_t *b = ui_label(s_modal, &font_txt_14, C_DIM, body);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_CENTER, 0, -82);

    // Up to 0..999: kPa limits are whole numbers to 500.
    static EXT_RAM_BSS_ATTR char whole[4000];
    int n = 0;
    for (int i = 0; i <= max_whole && n < (int)sizeof(whole) - 6; i++) {
        n += snprintf(whole + n, sizeof(whole) - n, i ? "\n%d" : "%d", i);
    }
    int w, f = 0;
    if (tenths) {
        w = (int)initial;
        f = (int)lroundf((initial - w) * 10);
        if (f == 10) { w++; f = 0; }
    } else {
        w = (int)lroundf(initial);
    }
    if (w > max_whole) w = max_whole;
    s_pick_whole = picker_roller(whole, w < 0 ? 0 : w, tenths ? -44 : -14, tenths ? 76 : 96);
    s_pick_tenth = tenths ? picker_roller(".0\n.1\n.2\n.3\n.4\n.5\n.6\n.7\n.8\n.9", f < 0 ? 0 : f, 30, 60) : NULL;
    lv_obj_t *u = ui_label(s_modal, &font_txt_18, C_DIM, unit);
    lv_obj_align(u, LV_ALIGN_CENTER, tenths ? 82 : 62, 2);

    // Below the rollers, lower than ui_confirm()'s buttons.
    lv_obj_t *cancel = modal_button(s_modal, "Cancel", C_DIM, -60, modal_cancel);
    lv_obj_t *ok = modal_button(s_modal, "Save", C_ACCENT, 60, picker_ok);
    lv_obj_align(cancel, LV_ALIGN_CENTER, -60, 96);
    lv_obj_align(ok, LV_ALIGN_CENTER, 60, 96);
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
    fade(s_modal, 0, LV_OPA_COVER, 180, 0);
}

// ---- Sheet ------------------------------------------------------------------
// A full-screen view the caller fills (a ride's stats). It's the modal, so the
// BOOT button closes it and the parked screen waits, as for a dialog.

lv_obj_t *ui_sheet(void)
{
    if (s_modal) {
        lv_obj_delete(s_modal);
    }
    s_modal = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_modal, C_BG, 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_CLICKABLE);   // swallows touches behind it
    lv_obj_t *col = ui_column(s_modal);
    lv_obj_add_flag(lv_layer_top(), LV_OBJ_FLAG_CLICKABLE);
    fade(s_modal, 0, LV_OPA_COVER, 180, 0);
    return col;
}

void ui_sheet_close(void) { modal_close(); }

// ---- Views ----------------------------------------------------------------------
// Pages that live behind a button on Status instead of in the pager. Each one
// is built at boot in its own full-screen box over the tileview, below the
// segments, pill and battery (built after it) and below everything on the top
// layer (sheets, dialogs, alerts, parked).

void ui_view_open(const ui_page_t *page)
{
    for (int i = 0; i < N_VIEWS; i++) {
        if (VIEWS[i] == page) {
            s_view = i;
            lv_obj_set_style_opa(s_views[i], 0, 0);
            show_fade_in(s_views[i], 180);
        }
    }
}

void ui_view_close(void)
{
    if (s_view < 0) {
        return;
    }
    fade_out_hide(s_views[s_view], 180);
    s_view = -1;
}

static void view_gesture(lv_event_t *e)
{
    if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_RIGHT) {
        ui_tick_feedback();
        ui_view_close();
    }
}

static lv_obj_t *build_view(const ui_page_t *page)
{
    lv_obj_t *o = lv_obj_create(s_main);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, UI_W, UI_H);
    lv_obj_set_style_bg_color(o, C_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);   // swallows touches meant for the pager behind it
    lv_obj_add_event_cb(o, view_gesture, LV_EVENT_GESTURE, NULL);
    page->create(o);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

static bool s_modal_open(void)
{
    return s_modal && !lv_obj_has_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
}

// ---- Parked screen ------------------------------------------------------------

static void parked_wake(void)
{
    // A touch or the BOOT button wakes the pages for another two minutes.
    s_not_live_since = lv_tick_get();
    s_parked_shown = false;
    fade_out_hide(s_parked, 300);
}

static void parked_touch(lv_event_t *e) { parked_wake(); }

static lv_obj_t *build_parked(void)
{
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, UI_W, UI_H);
    lv_obj_set_style_bg_color(o, C_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, parked_touch, LV_EVENT_CLICKED, NULL);
    lv_obj_t *img = lv_image_create(o);
    lv_image_set_src(img, &splash_bike);
    lv_obj_center(img);
    lv_obj_set_style_image_opa(img, LV_OPA_50, 0);
    lv_obj_t *l = ui_label(o, &font_txt_18, C_DIM, "Ignition off");
    lv_obj_align(l, LV_ALIGN_CENTER, 0, 118);
    s_parked_batt = ui_label(o, &font_txt_14, C_DIM, "");
    lv_obj_align(s_parked_batt, LV_ALIGN_CENTER, 0, 144);
    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    return o;
}

// ---- Battery saver --------------------------------------------------------------
// A dim black screen with the ride's status, for a board nobody is watching
// (in a tank bag). It is its own LVGL screen, so nothing on the pages is
// drawn while it shows, and its values change every 30 s. Alerts show on it at
// once and light it up for a few seconds. A tap or the BOOT button peeks at the
// pages; 30 s untouched, it comes back. This is an IPS panel: black pixels
// alone save nothing, the backlight and not drawing do.

static void saver_peek(void)
{
    s_peek = true;
    lv_display_trigger_activity(NULL);
}

static void saver_touch(lv_event_t *e)
{
    ui_tick_feedback();
    saver_peek();
    lv_timer_ready(s_frame);   // now, not on the next 1 s tick
}

static lv_obj_t *build_saver(void)
{
    lv_obj_t *o = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(o, C_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(o, saver_touch, LV_EVENT_CLICKED, NULL);
    s_sv_state = ui_label(o, &font_txt_18, C_DIM, "");
    lv_obj_set_style_text_letter_space(s_sv_state, 2, 0);
    lv_obj_align(s_sv_state, LV_ALIGN_CENTER, 0, -108);
    s_sv_time = ui_label(o, &font_num_48, C_TEXT, "");
    lv_obj_align(s_sv_time, LV_ALIGN_CENTER, 0, -48);
    s_sv_dist = ui_label(o, &font_txt_24, C_DIM, "");
    lv_obj_align(s_sv_dist, LV_ALIGN_CENTER, 0, 4);
    s_sv_tyres = ui_label(o, &font_txt_18, C_DIM, "");
    lv_obj_align(s_sv_tyres, LV_ALIGN_CENTER, 0, 44);
    s_sv_alert = ui_label(o, &font_txt_18, C_WARN, "");
    lv_obj_set_style_text_align(s_sv_alert, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_sv_alert, LV_ALIGN_CENTER, 0, 92);
    s_sv_batt = ui_label(o, &font_txt_14, C_DIM, "");
    lv_obj_align(s_sv_batt, LV_ALIGN_CENTER, 0, 140);
    return o;
}

static const char *phase_name(const dash_t *d, lv_color_t *color)
{
    *color = C_DIM;
    if (d->demo) {
        *color = C_ACCENT;
        return "DEMO";
    }
    switch (d->obd.phase) {
    case PHASE_NO_BLE: *color = C_ALERT; return "NO BLUETOOTH";
    case PHASE_SCAN: return "SEARCHING";
    case PHASE_CONNECT: return "CONNECTING";
    case PHASE_INIT: return "STARTING";
    case PHASE_ECU_OFF: *color = C_WARN; return "IGNITION OFF";
    case PHASE_LIVE: *color = C_OK; return "CONNECTED";
    }
    return "";
}

// "Tyres OK", or the first tyre that needs a look; "" with no tyre paired.
static lv_color_t saver_tyres(const dash_t *d, char *out, size_t n)
{
    const tpms_view_t *tp = &d->tpms;
    tpms_unit_t u = (tpms_unit_t)app_settings()->pressure_unit;
    int paired = 0;
    out[0] = 0;
    if (!tp->on) {
        return C_DIM;
    }
    for (int t = 0; t < TPMS_TYRES; t++) {
        if (!tp->pair[t].format) continue;
        paired++;
        const tpms_tyre_t *y = &tp->tyre[t];
        const char *name = t == TPMS_REAR ? "Rear" : "Front";
        if (isnan(y->kpa) || tpms_tyre_age(y, d->now) >= TPMS_STALE_MS) {
            snprintf(out, n, "%s no signal", name);
            return C_WARN;
        }
        if (tp->cfg.enabled && tp->cfg.low_on[t] && y->kpa < tp->cfg.low_kpa[t]) {
            char p[12];
            tpms_fmt_pressure(p, sizeof(p), y->kpa, u);
            snprintf(out, n, "%s low %s %s", name, p, tpms_unit_name(u));
            return C_WARN;
        }
    }
    if (paired) snprintf(out, n, "Tyres OK");
    return C_DIM;
}

static void saver_update(const dash_t *d)
{
    char buf[48];
    lv_color_t c;
    s_saver_drawn = d->now;
    ui_label_set(s_sv_state, phase_name(d, &c));
    ui_label_color(s_sv_state, c);
    uint32_t m = d->trip.engine_s / 60;
    snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(m / 60), (unsigned)(m % 60));
    ui_label_set(s_sv_time, buf);
    snprintf(buf, sizeof(buf), "%.1f km", d->trip.distance_m / 1000.0);
    ui_label_set(s_sv_dist, buf);
    c = saver_tyres(d, buf, sizeof(buf));
    ui_label_set(s_sv_tyres, buf);
    ui_label_color(s_sv_tyres, c);
    const battery_info_t *b = &d->battery;
    if (batt_present(d)) {
        snprintf(buf, sizeof(buf), "Board %u%%  %+d mA", b->soc_percent, b->current_ma);
    } else {
        snprintf(buf, sizeof(buf), "%s", d->battery_ok ? "USB power" : "");
    }
    ui_label_set(s_sv_batt, buf);
    ui_label_color(s_sv_batt, batt_present(d) && !b->charging && b->soc_percent <= 20 ? C_WARN : C_DIM);
}

// A new alert: its line, at once, and the backlight up for a few seconds.
static void saver_alert(const dash_t *d)
{
    char title[40], detail[48], buf[96];
    lv_color_t color;
    if (!alert_text(d, title, sizeof(title), detail, sizeof(detail), &color)) {
        return;
    }
    snprintf(buf, sizeof(buf), "%s%s%s", title, detail[0] ? "\n" : "", detail);
    ui_label_set(s_sv_alert, buf);
    ui_label_color(s_sv_alert, color);
    s_saver_bright_until = d->now + ALERT_SHOW_MS;
}

static void saver_enter(const dash_t *d)
{
    s_saver_on = true;
    // An alert still on the overlay moves to the saver screen (the 20 % battery
    // alert turns saver on in the same second).
    bool fresh = s_alert_until && d->now < s_alert_until;
    ui_label_set(s_sv_alert, "");
    s_saver_bright_until = 0;
    if (fresh) saver_alert(d);
    // The parked screen and an alert overlay would sit over it on the top layer.
    if (s_parked_shown) {
        s_parked_shown = false;
        lv_anim_delete(s_parked, NULL);
        lv_obj_add_flag(s_parked, LV_OBJ_FLAG_HIDDEN);
    }
    s_alert_until = 0;
    lv_anim_delete(s_alert, NULL);
    lv_obj_add_flag(s_alert, LV_OBJ_FLAG_HIDDEN);
    saver_update(d);
    lv_screen_load(s_saver);
    lv_timer_set_period(s_frame, SAVER_TICK_MS);
}

static void saver_leave(void)
{
    s_saver_on = false;
    lv_screen_load(s_main);
    lv_timer_set_period(s_frame, FRAME_MS);
    // A peek shows the pages, not the parked screen straight away.
    s_not_live_since = lv_tick_get();
}

// ---- BOOT button --------------------------------------------------------------
// Turns pages without swiping: press = next, double press = previous (both
// wrap), hold = the page's long-press action. The button driver calls back from
// the esp_timer task, so the callbacks only set bits that frame_cb() handles.

enum { BTN_NEXT = 1 << 0, BTN_PREV = 1 << 1, BTN_HOLD = 1 << 2 };
static _Atomic uint32_t s_btn_pending;

static void btn_cb(void *handle, void *arg)
{
    atomic_fetch_or(&s_btn_pending, (uint32_t)(uintptr_t)arg);
}

static void boot_button_init(void)
{
    const button_config_t cfg = {.long_press_time = 700, .short_press_time = 250};
    const button_gpio_config_t gpio = {.gpio_num = BSP_BUTTONS_IO_0, .active_level = 0};
    button_handle_t btn;
    esp_err_t err = iot_button_new_gpio_device(&cfg, &gpio, &btn);
    if (err != ESP_OK) {
        ESP_LOGW("ui", "BOOT button not available (%s)", esp_err_to_name(err));
        return;
    }
    iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, NULL, btn_cb, (void *)BTN_NEXT);
    iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK, NULL, btn_cb, (void *)BTN_PREV);
    iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, btn_cb, (void *)BTN_HOLD);
}

static void handle_button(uint32_t bits)
{
    ESP_LOGI("ui", "BOOT button:%s%s%s", bits & BTN_NEXT ? " press" : "", bits & BTN_PREV ? " double" : "",
             bits & BTN_HOLD ? " hold" : "");
    // Whatever covers the pages takes the press first. Confirming a dialog
    // stays a touch, so the button can never reset, clear or format by itself.
    if (s_saver_on) {
        saver_peek();
        return;
    }
    if (s_parked_shown) {
        parked_wake();
        return;
    }
    if (s_modal_open()) {
        if (bits & BTN_NEXT) modal_close();
        return;
    }
    if (s_alert_until && !lv_obj_has_flag(s_alert, LV_OBJ_FLAG_HIDDEN)) {
        s_alert_until = 0;
        return;
    }
    if (s_view >= 0) {
        if (bits & BTN_HOLD) {
            if (VIEWS[s_view]->hold) VIEWS[s_view]->hold();
        } else if (bits & BTN_NEXT) {
            ui_view_close();
            ui_tick_feedback();
        }
        return;
    }
    // The active tile changes as soon as set_tile is called, so two quick
    // presses move two pages even while the first slide is still running.
    lv_obj_t *act = lv_tileview_get_tile_active(s_tv);
    int cur = act ? (int)lv_obj_get_index(act) : (int)roundf(lv_obj_get_scroll_x(s_tv) / (float)UI_W);
    int next = cur;
    if (bits & BTN_NEXT) next = (next + 1) % N_PAGES;
    if (bits & BTN_PREV) next = (next + N_PAGES - 1) % N_PAGES;
    if (next != cur) {
        lv_tileview_set_tile_by_index(s_tv, next, 0, LV_ANIM_ON);
        ui_tick_feedback();
    }
    if ((bits & BTN_HOLD) && PAGES[cur]->hold) {
        PAGES[cur]->hold();
    }
}

// ---- Main screen --------------------------------------------------------------

static void build_main(void)
{
    s_main = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_main, C_BG, 0);
    lv_obj_set_style_bg_opa(s_main, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_main, LV_OBJ_FLAG_SCROLLABLE);

    s_tv = lv_tileview_create(s_main);
    lv_obj_set_size(s_tv, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_tv, C_BG, 0);
    lv_obj_set_style_bg_opa(s_tv, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_tv, LV_SCROLLBAR_MODE_OFF);
    for (int i = 0; i < N_PAGES; i++) {
        lv_dir_t dir = i == 0 ? LV_DIR_RIGHT : i == N_PAGES - 1 ? LV_DIR_LEFT : LV_DIR_HOR;
        lv_obj_t *tile = lv_tileview_add_tile(s_tv, i, 0, dir);
        lv_obj_set_style_bg_opa(tile, LV_OPA_TRANSP, 0);
        lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
        PAGES[i]->create(tile);
    }
    // The active tile stays NULL until the first swipe unless it's set here, and
    // the BOOT button reads it.
    lv_tileview_set_tile_by_index(s_tv, 0, 0, LV_ANIM_OFF);
    for (int i = 0; i < N_VIEWS; i++) {
        s_views[i] = build_view(VIEWS[i]);
    }

    // Page segments: LVGL angles run clockwise from 3 o'clock, so 90 is straight
    // down and larger angles are further left.
    for (int i = 0; i < N_PAGES; i++) {
        int end = SEG_LEFT - i * (SEG_DEG + GAP_DEG);
        s_segs[i] = ui_arc(s_main, 348, 4, end - SEG_DEG, SEG_DEG, C_FAINT, C_FAINT);
    }

    // Board battery icon, in the middle of the segments' curve: outline, nub, fill.
    s_batt = ui_box(s_main, 27, 12);
    lv_obj_align(s_batt, LV_ALIGN_BOTTOM_MID, 0, -17);
    s_batt_body = ui_box(s_batt, 24, 12);
    lv_obj_align(s_batt_body, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_radius(s_batt_body, 3, 0);
    lv_obj_set_style_border_width(s_batt_body, 2, 0);
    lv_obj_set_style_border_color(s_batt_body, C_DIM, 0);
    s_batt_nub = ui_box(s_batt, 2, 5);
    lv_obj_align(s_batt_nub, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_radius(s_batt_nub, 1, 0);
    lv_obj_set_style_bg_opa(s_batt_nub, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_batt_nub, C_DIM, 0);
    s_batt_fill = ui_box(s_batt_body, BATT_FILL_W, 6);
    lv_obj_align(s_batt_fill, LV_ALIGN_LEFT_MID, 3, 0);
    lv_obj_set_style_radius(s_batt_fill, 1, 0);
    lv_obj_set_style_bg_opa(s_batt_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_batt_fill, C_OK, 0);
    lv_obj_add_flag(s_batt, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *batt_hit = ui_box(s_main, 80, 44);
    lv_obj_align(batt_hit, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(batt_hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(batt_hit, on_batt_tap, LV_EVENT_CLICKED, NULL);

    s_pill = ui_box(s_main, LV_SIZE_CONTENT, 22);
    lv_obj_set_flex_flow(s_pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_pill, 6, 0);
    lv_obj_set_style_pad_hor(s_pill, 10, 0);
    lv_obj_set_style_radius(s_pill, 11, 0);
    lv_obj_set_style_bg_color(s_pill, C_BG, 0);
    lv_obj_set_style_bg_opa(s_pill, LV_OPA_80, 0);
    lv_obj_align(s_pill, LV_ALIGN_TOP_MID, 0, 22);
    s_pill_dot = ui_box(s_pill, 8, 8);
    lv_obj_set_style_radius(s_pill_dot, 4, 0);
    lv_obj_set_style_bg_opa(s_pill_dot, LV_OPA_COVER, 0);
    s_pill_text = ui_label(s_pill, &font_txt_14, C_DIM, "");
    lv_obj_set_style_text_letter_space(s_pill_text, 2, 0);
    lv_obj_add_flag(s_pill, LV_OBJ_FLAG_HIDDEN);

    s_toast = ui_box(lv_layer_top(), LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(s_toast, 16, 0);
    lv_obj_set_style_pad_ver(s_toast, 8, 0);
    lv_obj_set_style_radius(s_toast, 18, 0);
    lv_obj_set_style_bg_color(s_toast, C_TRACK_HI, 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_align(s_toast, LV_ALIGN_CENTER, 0, 104);
    s_toast_text = ui_label(s_toast, &font_txt_18, C_TEXT, "");
    lv_obj_set_style_text_align(s_toast_text, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    s_parked = build_parked();
    s_alert = build_alert();
    s_saver = build_saver();
}

// Over everything, for the 3 s before the board sleeps.
static void show_switching_off(guard_action_t why)
{
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, UI_W, UI_H);
    lv_obj_set_style_bg_color(o, C_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);   // swallows touches
    lv_obj_t *l = ui_label(o, &font_txt_18, C_ALERT, why == GUARD_OFF_HOT ? "Board too hot" : "Board battery empty");
    lv_obj_align(l, LV_ALIGN_CENTER, 0, -14);
    lv_obj_t *s = ui_label(o, &font_txt_14, C_DIM, "ride saved, switching off");
    lv_obj_align(s, LV_ALIGN_CENTER, 0, 14);
}

static void frame_cb(lv_timer_t *t)
{
    uint32_t now_tick = lv_tick_get();
    float dt = s_last_frame ? lv_tick_diff(now_tick, s_last_frame) / 1000.0f : FRAME_MS / 1000.0f;
    if (dt > 0.2f) dt = 0.2f;
    s_last_frame = now_tick;
    app_snapshot(&s_dash);
    const dash_t *d = &s_dash;
    if (d->switching_off) {
        if (!s_off_shown) {
            s_off_shown = true;
            show_switching_off((guard_action_t)d->switching_off);
        }
        set_brightness(app_settings()->brightness < 40 ? app_settings()->brightness : 40);
        return;
    }

    uint32_t buttons = atomic_exchange(&s_btn_pending, 0);
    if (buttons) {
        handle_button(buttons);
    }

    // Battery saver: a peek ends after PEEK_MS untouched or with the setting.
    const settings_t *st = app_settings();
    if (s_peek && (!st->saver || lv_display_get_inactive_time(NULL) >= PEEK_MS)) {
        s_peek = false;
    }
    bool saver = st->saver && !s_peek;
    if (saver != s_saver_on) {
        if (saver) saver_enter(d);
        else saver_leave();
    }
    if (s_saver_on) {
        if (d->alert_seq != s_last_alert_seq) {
            s_last_alert_seq = d->alert_seq;
            saver_alert(d);
        }
        if (d->now - s_saver_drawn >= SAVER_UPDATE_MS) {
            saver_update(d);
        }
        int full = st->brightness;
        set_brightness(d->now < s_saver_bright_until || full < SAVER_BRIGHTNESS ? full : SAVER_BRIGHTNESS);
        log_fps();
        return;
    }

    // Behind the parked screen nothing on the pages needs drawing.
    if (!s_parked_shown) {
        float pos = lv_obj_get_scroll_x(s_tv) / (float)UI_W;
        int page = (int)roundf(pos);
        for (int i = 0; i < N_PAGES; i++) {
            PAGES[i]->update(d, dt, s_view < 0 && fabsf(pos - i) < 1.0f);
        }
        for (int i = 0; i < N_VIEWS; i++) {
            VIEWS[i]->update(d, dt, s_view == i);
        }
        update_segments();
        update_battery(d);
        update_pill(d, page);
        s_last_page = page;
    }
    log_fps();

    if (d->alert_seq != s_last_alert_seq) {
        s_last_alert_seq = d->alert_seq;
        alert_show(d);
    }
    if (s_alert_until && d->now > s_alert_until) {
        s_alert_until = 0;
    }
    if (!s_alert_until && !lv_obj_has_flag(s_alert, LV_OBJ_FLAG_HIDDEN) && !lv_anim_get(s_alert, anim_opa_cb)) {
        fade_out_hide(s_alert, 300);
    }

    // Parked: two minutes without the ECU (not in demo) dims to the bike.
    bool live = d->demo || d->obd.phase == PHASE_LIVE;
    if (live) {
        s_not_live_since = now_tick;
        if (s_parked_shown) {
            s_parked_shown = false;
            fade_out_hide(s_parked, 400);
        }
    } else if (!s_parked_shown && lv_tick_diff(now_tick, s_not_live_since) > PARKED_AFTER_MS && !s_modal_open()) {
        s_parked_shown = true;
        lv_obj_set_style_opa(s_parked, 0, 0);
        show_fade_in(s_parked, 800);
    }
    if (s_parked_shown) {
        update_parked_battery(d);
    }
    // Parked dims; otherwise the slider's level (its live preview included).
    set_brightness(s_parked_shown ? 30 : st->brightness);
}

// Counts display refreshes; the frame timer logs the rate every 10 s.
static void on_refresh(lv_event_t *e)
{
    s_refreshes++;
}

static void log_fps(void)
{
    uint32_t now = lv_tick_get();
    if (!s_fps_from) {
        s_fps_from = now;
        return;
    }
    uint32_t span = lv_tick_diff(now, s_fps_from);
    if (span >= 10000) {
        ESP_LOGI("ui", "%.1f fps (page %d)", s_refreshes * 1000.0f / span, s_last_page + 1);
        s_refreshes = 0;
        s_fps_from = now;
    }
}

#if CONFIG_DASH_SCREENSHOTS
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
// Development: show each page, let its values settle, print it over serial.
// The printing (seconds over USB) runs in its own task so LVGL keeps going.
static lv_draw_buf_t *volatile s_shot_buf;
static volatile int s_shot_page;

static void shot_print_task(void *arg)
{
    for (;;) {
        lv_draw_buf_t *buf = s_shot_buf;
        if (!buf) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        printf("\nSHOT %d %d %d %d\n", s_shot_page, (int)buf->header.w, (int)buf->header.h,
               (int)buf->header.stride);
        size_t len = buf->header.stride * buf->header.h;
        unsigned char line[80];
        for (size_t i = 0; i < len; i += 57) {
            size_t olen = 0, n = len - i < 57 ? len - i : 57;
            mbedtls_base64_encode(line, sizeof(line), &olen, buf->data + i, n);
            fwrite(line, 1, olen, stdout);
            fputc('\n', stdout);
            if ((i / 57) % 64 == 0) vTaskDelay(1);
        }
        printf("ENDSHOT\n");
        fflush(stdout);
        s_shot_buf = NULL;   // the LVGL side frees it
    }
}

static void shot_cb(lv_timer_t *t)
{
    static int step;
    static lv_draw_buf_t *done;
    if (s_shot_buf || lv_tick_get() < CONFIG_DASH_SCREENSHOT_DELAY_S * 1000) {
        return;   // still printing, or not yet
    }
    if (done) {
        lv_draw_buf_destroy(done);
        done = NULL;
    }
    if (step / 2 >= N_PAGES + N_VIEWS) {
        step = 0;   // lap again: grab_screens.py picks up any page it missed
    }
    // The pages, then the views (numbered after them), opened over Status.
    int page = step / 2;
    if (step % 2 == 0) {
        if (s_view >= 0) {
            lv_obj_add_flag(s_views[s_view], LV_OBJ_FLAG_HIDDEN);
            s_view = -1;
        }
        if (page < N_PAGES) {
            lv_tileview_set_tile_by_index(s_tv, page, 0, LV_ANIM_OFF);
        } else {
            s_view = page - N_PAGES;
            lv_obj_set_style_opa(s_views[s_view], LV_OPA_COVER, 0);
            lv_obj_remove_flag(s_views[s_view], LV_OBJ_FLAG_HIDDEN);
        }
    } else {
        done = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB565);
        s_shot_page = page + 1;
        s_shot_buf = done;
    }
    step++;
}
#endif

#if CONFIG_DASH_AUTOTOUR
static void tour_cb(lv_timer_t *t)
{
    static int next = 1;
    lv_tileview_set_tile_by_index(s_tv, next, 0, LV_ANIM_ON);
    next = (next + 1) % N_PAGES;
}
#endif

// ---- Splash -------------------------------------------------------------------

static lv_obj_t *s_splash;

static void splash_done(lv_timer_t *t)
{
    // Straight to the saver screen if it was on: the fade ends on it, and the
    // first frame finds it loaded.
    s_saver_on = app_settings()->saver;
    s_saver_drawn = -SAVER_UPDATE_MS;   // its values are due on the first frame
    lv_screen_load_anim(s_saver_on ? s_saver : s_main, LV_SCREEN_LOAD_ANIM_FADE_IN, 450, 0, true);
    s_not_live_since = lv_tick_get();
    s_frame = lv_timer_create(frame_cb, s_saver_on ? SAVER_TICK_MS : FRAME_MS, NULL);
    lv_display_add_event_cb(lv_display_get_default(), on_refresh, LV_EVENT_REFR_READY, NULL);
    boot_button_init();   // only once the pages are up
#if CONFIG_DASH_AUTOTOUR
    lv_timer_create(tour_cb, 5000, NULL);
#endif
#if CONFIG_DASH_SCREENSHOTS
    esp_log_level_set("*", ESP_LOG_ERROR);   // keep other tasks' logs out of the image data
    xTaskCreatePinnedToCore(shot_print_task, "shots", 4096, NULL, 1, NULL, 1);
    lv_timer_create(shot_cb, 1500, NULL);
#endif
}

static void splash_build_main(lv_timer_t *t)
{
    // Built while the splash plays, so the switch is instant.
    build_main();
    mem_checkpoint("pages built");
    lv_timer_t *done = lv_timer_create(splash_done, SPLASH_MIN_MS - 100, NULL);
    lv_timer_set_repeat_count(done, 1);
}

static void show_splash(void)
{
    s_splash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_splash, C_BG, 0);
    lv_obj_set_style_bg_opa(s_splash, LV_OPA_COVER, 0);
    lv_obj_remove_flag(s_splash, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *img = lv_image_create(s_splash);
    lv_image_set_src(img, &splash_bike);
    lv_obj_center(img);
    lv_obj_set_style_opa(img, 0, 0);

    lv_obj_t *title = ui_label(s_splash, &font_txt_24, lv_color_hex(0xC9CED6), BIKE_NAME);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 118);
    lv_obj_set_style_opa(title, 0, 0);

    lv_obj_t *ring = lv_arc_create(s_splash);
    lv_obj_set_size(ring, 352, 352);
    lv_obj_center(ring);
    lv_obj_remove_style(ring, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_opa(ring, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(ring, 3, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(ring, C_ACCENT, LV_PART_INDICATOR);
    lv_arc_set_bg_angles(ring, 0, 360);
    lv_arc_set_angles(ring, 270, 270);

    lv_screen_load(s_splash);

    // Backlight up, bike fades in, the name spreads out, the rim sweeps once.
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, anim_brightness_cb);
    lv_anim_set_values(&a, 0, app_settings()->brightness);
    lv_anim_set_duration(&a, 700);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    fade(img, 0, LV_OPA_COVER, 700, 100);
    fade(title, 0, LV_OPA_COVER, 500, 650);
    lv_anim_init(&a);
    lv_anim_set_var(&a, title);
    lv_anim_set_exec_cb(&a, anim_letter_cb);
    lv_anim_set_values(&a, 0, 10);
    lv_anim_set_duration(&a, 900);
    lv_anim_set_delay(&a, 650);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
    lv_anim_init(&a);
    lv_anim_set_var(&a, ring);
    lv_anim_set_exec_cb(&a, anim_arc_cb);
    lv_anim_set_values(&a, 0, 360);
    lv_anim_set_duration(&a, 1300);
    lv_anim_set_delay(&a, 700);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    lv_timer_t *t = lv_timer_create(splash_build_main, 120, NULL);
    lv_timer_set_repeat_count(t, 1);
}

void ui_start(void)
{
    bsp_display_lock(-1);
#if !CONFIG_DASH_PERF_MONITOR
    lv_sysmon_hide_performance(NULL);
#endif
    show_splash();
    bsp_display_unlock();
}
