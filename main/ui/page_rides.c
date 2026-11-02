/*
 * Rides, opened from the Status page: the rides on the microSD card, newest
 * first, by number. Tap one for its stats, from the ride.json the SD writer
 * task reads back (sd_log_rides). Scrolls vertically.
 */
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "ui_common.h"
#include "sd_log.h"

#define REFRESH_MS 30000   // the open ride's summary is rewritten every 30 s
#define POLL_MS    500

static lv_obj_t *s_col, *s_note, *s_list;
static ride_summary_t *s_new, *s_shown;   // PSRAM, SD_RIDES_MAX each
static int s_n = -1, s_live = -1;
static uint32_t s_seq;
static bool s_was_visible;
static int64_t s_last_refresh, s_last_poll;

static bool has(const ride_summary_t *r, uint32_t bit) { return (r->have & bit) != 0; }

static void fmt_hm(char *buf, size_t n, bool ok, uint32_t secs)
{
    if (ok) snprintf(buf, n, "%u:%02u", (unsigned)(secs / 3600), (unsigned)(secs / 60 % 60));
    else snprintf(buf, n, "--");
}

static void fmt_km(char *buf, size_t n, const ride_summary_t *r)
{
    if (has(r, RS_DISTANCE)) snprintf(buf, n, "%.1f km", r->km);
    else snprintf(buf, n, "-- km");
}

// ---- One ride's stats ---------------------------------------------------------

static void on_back(lv_event_t *e)
{
    ui_tick_feedback();
    ui_sheet_close();
}

static void cell(lv_obj_t *grid, const char *value, lv_color_t color, const char *caption)
{
    lv_obj_t *c = ui_box(grid, 125, 58);
    lv_obj_t *v = ui_label(c, &font_num_32, color, value);
    lv_obj_align(v, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_t *l = ui_label(c, &font_txt_14, C_DIM, caption);
    lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 36);
}

static void kv(lv_obj_t *col, const char *caption, const char *value, lv_color_t color)
{
    lv_obj_t *r = ui_box(col, 250, LV_SIZE_CONTENT);
    lv_obj_t *c = ui_label(r, &font_txt_14, C_DIM, caption);
    lv_obj_align(c, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *v = ui_label(r, &font_txt_14, color, value);
    lv_obj_set_width(v, 150);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(v, LV_ALIGN_TOP_RIGHT, 0, 0);
}

static void show_ride(const ride_summary_t *r, bool live)
{
    lv_obj_t *col = ui_sheet();
    char buf[64];
    snprintf(buf, sizeof(buf), "RIDE %d", r->ride);
    lv_obj_t *cap = ui_label(col, &font_txt_14, C_ACCENT, buf);
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    // A summary still marked "in progress" on a closed ride was last saved
    // mid-ride: the board lost power before the ride ended.
    if (live || r->in_progress) {
        ui_label(col, &font_txt_14, live ? C_OK : C_DIM, live ? "in progress" : "last saved mid-ride");
    }
    fmt_km(buf, sizeof(buf), r);
    ui_label(col, &font_num_48, C_ACCENT, buf);

    lv_obj_t *grid = ui_box(col, 250, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(grid, 6, 0);
    fmt_hm(buf, sizeof(buf), has(r, RS_DURATION), r->duration_s);
    cell(grid, buf, C_TEXT, "DURATION");
    fmt_hm(buf, sizeof(buf), has(r, RS_ENGINE), r->engine_s);
    cell(grid, buf, C_TEXT, "ENGINE TIME");
    snprintf(buf, sizeof(buf), has(r, RS_AVG) ? "%.0f" : "--", r->avg_kmh);
    cell(grid, buf, C_TEXT, "AVG km/h");
    snprintf(buf, sizeof(buf), has(r, RS_MAX_KMH) ? "%.0f" : "--", r->max_kmh);
    cell(grid, buf, C_TEXT, "MAX km/h");
    if (has(r, RS_MAX_RPM)) ui_fmt_thousands(buf, sizeof(buf), (int)(r->max_rpm + 0.5f));
    else snprintf(buf, sizeof(buf), "--");
    cell(grid, buf, C_TEXT, "MAX RPM");
    bool cool = has(r, RS_MAX_COOLANT) && r->max_coolant > 0;
    snprintf(buf, sizeof(buf), cool ? "%.0f\xC2\xB0" : "--", r->max_coolant);
    cell(grid, buf, cool ? ui_coolant_color(r->max_coolant) : C_FAINT, "MAX COOLANT");
    bool volts = has(r, RS_MIN_VOLTS) && r->min_volts > 0;
    snprintf(buf, sizeof(buf), volts ? "%.1f" : "--", r->min_volts);
    cell(grid, buf, C_TEXT, "MIN VOLTS");
    snprintf(buf, sizeof(buf), has(r, RS_MAX_G) ? "%.2f" : "--", r->max_g);
    cell(grid, buf, C_TEXT, "MAX G");

    if (has(r, RS_LEAN)) snprintf(buf, sizeof(buf), "L %.0f\xC2\xB0  R %.0f\xC2\xB0", r->lean_left, r->lean_right);
    else snprintf(buf, sizeof(buf), "--");
    kv(col, "Max lean", buf, C_TEXT);
    snprintf(buf, sizeof(buf), has(r, RS_ALERTS) ? "%lu" : "--", (unsigned long)r->alerts);
    kv(col, "Alerts", buf, r->alerts ? C_WARN : C_TEXT);
    kv(col, "Fault codes", !has(r, RS_CODES) ? "--" : r->codes[0] ? r->codes : "none",
       r->codes[0] ? C_ALERT : C_TEXT);
    snprintf(buf, sizeof(buf), has(r, RS_FUEL) ? "%.1f L" : "--", r->fuel_raw_l * r->k);
    kv(col, "Fuel (est.)", buf, C_TEXT);
    snprintf(buf, sizeof(buf), has(r, RS_L100) ? "%.1f" : "--", r->l100_raw * r->k);
    kv(col, "L/100 km (est.)", buf, C_TEXT);
    if (!has(r, RS_BATTERY)) snprintf(buf, sizeof(buf), "--");
    else if (r->batt_start >= 0) snprintf(buf, sizeof(buf), "%d%% to %d%%", r->batt_start, r->batt_end);
    else snprintf(buf, sizeof(buf), "%d%% at the end", r->batt_end);
    kv(col, "Board battery", buf, C_TEXT);

    lv_obj_t *btn = lv_button_create(col);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 200, 40);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, C_FAINT, 0);
    lv_obj_set_style_bg_color(btn, C_TRACK_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_margin_top(btn, 8, 0);
    lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bt = ui_label(btn, &font_txt_18, C_DIM, "Back");
    lv_obj_center(bt);
}

// ---- The list -----------------------------------------------------------------

static void on_row(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    ui_tick_feedback();
    show_ride(&s_shown[i], s_shown[i].ride == s_live);
}

static void add_row(int i)
{
    const ride_summary_t *r = &s_shown[i];
    lv_obj_t *btn = lv_button_create(s_list);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 250, 60);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, C_TRACK_HI, 0);
    lv_obj_set_style_bg_color(btn, C_TRACK_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, on_row, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    char buf[64];
    snprintf(buf, sizeof(buf), "Ride %d", r->ride);
    lv_obj_t *title = ui_label(btn, &font_txt_18, C_TEXT, buf);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 8);
    if (r->ride == s_live) {
        lv_obj_t *tag = ui_label(btn, &font_txt_14, C_OK, "now");
        lv_obj_align_to(tag, title, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    }
    fmt_km(buf, sizeof(buf), r);
    lv_obj_t *km = ui_label(btn, &font_txt_18, C_ACCENT, buf);
    lv_obj_align(km, LV_ALIGN_TOP_RIGHT, -14, 8);

    char dur[12];
    fmt_hm(dur, sizeof(dur), has(r, RS_DURATION), r->duration_s);
    int n = snprintf(buf, sizeof(buf), "%s", dur);
    if (has(r, RS_MAX_KMH)) n += snprintf(buf + n, sizeof(buf) - n, "  max %.0f km/h", r->max_kmh);
    if (r->alerts) snprintf(buf + n, sizeof(buf) - n, "  %lu alert%s", (unsigned long)r->alerts, r->alerts > 1 ? "s" : "");
    lv_obj_t *sub = ui_label(btn, &font_txt_14, C_DIM, buf);
    lv_obj_set_width(sub, 222);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_align(sub, LV_ALIGN_BOTTOM_LEFT, 14, -8);
}

static void create(lv_obj_t *tile)
{
    s_new = heap_caps_calloc(SD_RIDES_MAX, sizeof(ride_summary_t), MALLOC_CAP_SPIRAM);
    s_shown = heap_caps_calloc(SD_RIDES_MAX, sizeof(ride_summary_t), MALLOC_CAP_SPIRAM);
    s_col = ui_column(tile);
    lv_obj_t *h = ui_back_caption(s_col, C_ACCENT, "RIDES");
    lv_obj_set_style_margin_top(h, 4, 0);
    s_note = ui_label(s_col, &font_txt_14, C_DIM, "Reading the card\xE2\x80\xA6");
    s_list = ui_box(s_col, 250, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
}

static void update(const dash_t *d, float dt, bool visible)
{
    if (!visible) {
        s_was_visible = false;
        return;
    }
    if (!s_was_visible || d->now - s_last_refresh >= REFRESH_MS) {
        s_was_visible = true;
        s_last_refresh = d->now;
        sd_log_rides_refresh();
    }
    if (d->now - s_last_poll < POLL_MS || !s_new || !s_shown) {
        return;
    }
    s_last_poll = d->now;
    uint32_t seq;
    int n = sd_log_rides(s_new, SD_RIDES_MAX, &seq);
    sd_status_t sd;
    sd_log_status(&sd);
    int live = sd.logging ? sd.ride : -1;
    const char *note = n < 0 ? "Reading the card\xE2\x80\xA6" : !sd.mounted ? "No SD card"
                     : n == 0 ? "No rides on the card yet" : "";
    ui_label_set(s_note, note);
    if (note[0]) lv_obj_remove_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_note, LV_OBJ_FLAG_HIDDEN);
    if (n < 0 || (seq == s_seq && live == s_live)) {
        return;
    }
    s_seq = seq;
    // Rebuild only when something changed, so the scroll position holds.
    bool same = n == s_n && live == s_live && memcmp(s_new, s_shown, n * sizeof(*s_new)) == 0;
    if (same) {
        return;
    }
    memcpy(s_shown, s_new, n * sizeof(*s_new));
    s_n = n;
    s_live = live;
    lv_obj_clean(s_list);
    for (int i = 0; i < n; i++) {
        add_row(i);
    }
}

const ui_page_t PAGE_RIDES = {create, update, NULL};
