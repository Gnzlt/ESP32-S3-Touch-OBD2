/*
 * Page 4, Performance: throttle, engine load and manifold pressure as three
 * concentric arcs; ignition advance in the middle; short-term fuel trim on a
 * centre-zero bar; the fuel system's loop state.
 */
#include <math.h>
#include <stdio.h>
#include "ui_common.h"
#include "obd/pids.h"

// 240 degrees, not 270: the arcs end higher, leaving the bottom of the
// circle free for the readouts (at 270 the arc ends sat on top of them).
#define START 150
#define SWEEP 240
#define MAP_MAX 105.0f   // kPa: a little over sea-level atmosphere

static lv_obj_t *s_thr, *s_load, *s_map, *s_adv, *s_trim_bar, *s_trim_mark, *s_trim_val, *s_loop;
static lv_obj_t *s_vthr, *s_vload, *s_vmap;
static smooth_t s_thr_v = {.tau = 0.12f}, s_load_v = {.tau = 0.15f}, s_map_v = {.tau = 0.15f};
static smooth_t s_adv_v = {.tau = 0.3f}, s_trim_v = {.tau = 0.3f};

#define TRIM_W 150

// Value and caption sit straight on the tile. Inside a tight box they stuck
// out of it, and LVGL clips drawing to the parent: the overflowing part was
// never redrawn, which left ghost digits and cut-off captions.
static void legend(lv_obj_t *tile, int x, lv_color_t color, const char *caption, lv_obj_t **value)
{
    *value = ui_label(tile, &font_num_32, C_TEXT, "--");
    lv_obj_align(*value, LV_ALIGN_CENTER, x, 98);
    lv_obj_t *c = ui_label(tile, &font_txt_14, color, caption);
    lv_obj_align(c, LV_ALIGN_CENTER, x, 124);
}

static void create(lv_obj_t *tile)
{
    s_thr = ui_arc(tile, 340, 12, START, SWEEP, C_TRACK, C_ACCENT);
    s_load = ui_arc(tile, 306, 12, START, SWEEP, C_TRACK, C_CYAN);
    s_map = ui_arc(tile, 272, 12, START, SWEEP, C_TRACK, C_VIOLET);

    lv_obj_t *cap = ui_label(tile, &font_txt_14, C_DIM, "ADVANCE");
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -70);
    s_adv = ui_label(tile, &font_num_48, C_TEXT, "--");
    lv_obj_align(s_adv, LV_ALIGN_CENTER, 0, -36);

    // Fuel trim: a centre-zero bar, -25 % .. +25 %.
    s_trim_bar = lv_obj_create(tile);
    lv_obj_remove_style_all(s_trim_bar);
    lv_obj_set_size(s_trim_bar, TRIM_W, 6);
    lv_obj_align(s_trim_bar, LV_ALIGN_CENTER, 0, 14);
    lv_obj_set_style_bg_color(s_trim_bar, C_TRACK_HI, 0);
    lv_obj_set_style_bg_opa(s_trim_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_trim_bar, 3, 0);
    lv_obj_t *zero = lv_obj_create(s_trim_bar);
    lv_obj_remove_style_all(zero);
    lv_obj_set_size(zero, 2, 6);
    lv_obj_center(zero);
    lv_obj_set_style_bg_color(zero, C_DIM, 0);
    lv_obj_set_style_bg_opa(zero, LV_OPA_COVER, 0);
    s_trim_mark = lv_obj_create(tile);
    lv_obj_remove_style_all(s_trim_mark);
    lv_obj_set_size(s_trim_mark, 10, 14);
    lv_obj_set_style_radius(s_trim_mark, 3, 0);
    lv_obj_set_style_bg_color(s_trim_mark, C_TEXT, 0);
    lv_obj_set_style_bg_opa(s_trim_mark, LV_OPA_COVER, 0);
    lv_obj_align(s_trim_mark, LV_ALIGN_CENTER, 0, 14);
    s_trim_val = ui_label(tile, &font_txt_14, C_DIM, "FUEL TRIM --");
    lv_obj_align(s_trim_val, LV_ALIGN_CENTER, 0, 36);
    s_loop = ui_label(tile, &font_txt_14, C_FAINT, "");
    lv_obj_align(s_loop, LV_ALIGN_CENTER, 0, 56);

    legend(tile, -74, C_ACCENT, "THROTTLE", &s_vthr);
    legend(tile, 0, C_CYAN, "LOAD", &s_vload);
    legend(tile, 74, C_VIOLET, "MAP kPa", &s_vmap);
}

static void set_pct(lv_obj_t *label, bool has, float v, const char *fmt)
{
    char buf[16];
    if (has) {
        snprintf(buf, sizeof(buf), fmt, (int)roundf(v));
        ui_label_set(label, buf);
        ui_label_color(label, C_TEXT);
    } else {
        ui_label_set(label, "--");
        ui_label_color(label, C_FAINT);
    }
}

static void update(const dash_t *d, float dt, bool visible)
{
    const obd_state_t *o = &d->obd;
    float thr = 0, load = 0, map = 0, adv = 0, trim = 0;
    bool h_thr = obd_value(o, PID_THROTTLE, d->now, &thr);
    bool h_load = obd_value(o, PID_LOAD, d->now, &load);
    bool h_map = obd_value(o, PID_MAP, d->now, &map);
    bool h_adv = obd_value(o, PID_ADVANCE, d->now, &adv);
    bool h_trim = obd_value(o, PID_STFT1, d->now, &trim);
    smooth_set(&s_thr_v, true, h_thr ? thr : 0);
    smooth_set(&s_load_v, true, h_load ? load : 0);
    smooth_set(&s_map_v, true, h_map ? map : 0);
    smooth_set(&s_adv_v, h_adv, adv);
    smooth_set(&s_trim_v, true, h_trim ? trim : 0);
    float t = smooth_step(&s_thr_v, dt), l = smooth_step(&s_load_v, dt), m = smooth_step(&s_map_v, dt);
    float a = smooth_step(&s_adv_v, dt), f = smooth_step(&s_trim_v, dt);
    if (!visible) {
        return;
    }
    ui_arc_set(s_thr, (int)(t * 10));
    ui_arc_set(s_load, (int)(l * 10));
    ui_arc_set(s_map, (int)(m / MAP_MAX * 1000));
    set_pct(s_vthr, h_thr, t, "%d%%");
    set_pct(s_vload, h_load, l, "%d%%");
    set_pct(s_vmap, h_map, m, "%d");

    char buf[32];
    if (h_adv) {
        snprintf(buf, sizeof(buf), "%.1f\xC2\xB0", a);
        ui_label_set(s_adv, buf);
        ui_label_color(s_adv, C_TEXT);
    } else {
        ui_label_set(s_adv, "--");
        ui_label_color(s_adv, C_FAINT);
    }
    float clamped = f < -25 ? -25 : f > 25 ? 25 : f;
    lv_obj_align(s_trim_mark, LV_ALIGN_CENTER, (int)(clamped / 25.0f * (TRIM_W / 2 - 5)), 14);
    lv_obj_set_style_bg_color(s_trim_mark, fabsf(f) > 10 ? C_WARN : C_TEXT, 0);
    if (h_trim) {
        snprintf(buf, sizeof(buf), "FUEL TRIM %+.1f%%", f);
    } else {
        snprintf(buf, sizeof(buf), "FUEL TRIM --");
    }
    ui_label_set(s_trim_val, buf);
    float fs;
    if (obd_value(o, PID_FUEL_STATUS, d->now, &fs)) {
        ui_label_set(s_loop, pid_fuel_status_text(o->raw[PID_FUEL_STATUS][0]));
    } else {
        ui_label_set(s_loop, "");
    }
}

const ui_page_t PAGE_PERF = {create, update, NULL};
