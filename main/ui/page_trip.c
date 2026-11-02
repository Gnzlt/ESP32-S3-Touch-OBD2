/*
 * Page 6, Trip: the ride so far. Distance up top, then time, speeds and the
 * engine's extremes. Saved every minute, so a reboot keeps it. Long-press to
 * reset (after a confirm).
 */
#include <math.h>
#include <stdio.h>
#include "ui_common.h"

typedef enum { T_TIME, T_AVG, T_MAXSPD, T_MAXRPM, T_MAXCOOL, T_MINV, T_COUNT } stat_id_t;

static lv_obj_t *s_dist, *s_val[T_COUNT];
static uint32_t s_last_hash = 1;

static void reset_trip(void)
{
    app_request(REQ_RESET_TRIP);
    ui_toast("Trip reset");
}

static void hold(void)
{
    ui_tick_feedback();
    ui_confirm("Reset trip?", "Distance, time and every\nmaximum start again.", "Reset", C_WARN, reset_trip);
}

static void on_long_press(lv_event_t *e) { hold(); }

static void cell(lv_obj_t *tile, stat_id_t id, int x, int y, const char *caption)
{
    s_val[id] = ui_label(tile, &font_num_32, C_TEXT, "--");
    lv_obj_align(s_val[id], LV_ALIGN_CENTER, x, y);
    lv_obj_t *c = ui_label(tile, &font_txt_14, C_DIM, caption);
    lv_obj_align(c, LV_ALIGN_CENTER, x, y + 24);
}

static void create(lv_obj_t *tile)
{
    lv_obj_add_event_cb(tile, on_long_press, LV_EVENT_LONG_PRESSED, NULL);
    lv_obj_t *cap = ui_label(tile, &font_txt_14, C_DIM, "TRIP");
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -128);
    s_dist = ui_label(tile, &font_num_48, C_ACCENT, "0.0 km");
    lv_obj_align(s_dist, LV_ALIGN_CENTER, 0, -92);

    lv_obj_t *line = ui_box(tile, 220, 1);
    lv_obj_set_style_bg_color(line, C_TRACK_HI, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_align(line, LV_ALIGN_CENTER, 0, -62);

    cell(tile, T_TIME, -64, -36, "ENGINE TIME");
    cell(tile, T_AVG, 64, -36, "AVG km/h");
    cell(tile, T_MAXSPD, -64, 22, "MAX km/h");
    cell(tile, T_MAXRPM, 64, 22, "MAX RPM");
    cell(tile, T_MAXCOOL, -64, 80, "MAX COOLANT");
    cell(tile, T_MINV, 64, 80, "MIN VOLTS");
    lv_obj_t *hint = ui_label(tile, &font_txt_14, C_FAINT, "hold to reset");
    lv_obj_align(hint, LV_ALIGN_CENTER, 0, 136);
}

static void update(const dash_t *d, float dt, bool visible)
{
    if (!visible) {
        return;
    }
    const trip_t *t = &d->trip;
    // Only re-format when something changed (cheap hash of the fields shown).
    uint32_t h = (uint32_t)(t->distance_m / 50) * 31u + t->engine_s * 7u + (uint32_t)t->max_kmh * 13u
                 + (uint32_t)t->max_rpm + (uint32_t)(t->max_coolant * 3) + (uint32_t)(t->min_volts * 100) * 17u;
    if (h == s_last_hash) {
        return;
    }
    s_last_hash = h;
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f km", t->distance_m / 1000.0);
    ui_label_set(s_dist, buf);
    snprintf(buf, sizeof(buf), "%u:%02u", (unsigned)(t->engine_s / 3600), (unsigned)(t->engine_s / 60 % 60));
    ui_label_set(s_val[T_TIME], buf);
    snprintf(buf, sizeof(buf), "%d", (int)roundf(trip_avg_kmh(t)));
    ui_label_set(s_val[T_AVG], buf);
    snprintf(buf, sizeof(buf), "%d", (int)roundf(t->max_kmh));
    ui_label_set(s_val[T_MAXSPD], buf);
    ui_fmt_thousands(buf, sizeof(buf), (int)roundf(t->max_rpm));
    ui_label_set(s_val[T_MAXRPM], buf);
    snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)roundf(t->max_coolant));
    ui_label_set(s_val[T_MAXCOOL], t->max_coolant > 0 ? buf : "--");
    ui_label_color(s_val[T_MAXCOOL], t->max_coolant > 0 ? ui_coolant_color(t->max_coolant) : C_FAINT);
    snprintf(buf, sizeof(buf), "%.1f", t->min_volts);
    ui_label_set(s_val[T_MINV], t->min_volts > 0 ? buf : "--");
}

const ui_page_t PAGE_TRIP = {create, update, hold};
