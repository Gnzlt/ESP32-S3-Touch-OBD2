/*
 * Page 2, Engine: coolant on a big gauge with its zones (cold, normal, warm,
 * hot), the last 10 minutes of coolant as a sparkline, battery / charging
 * voltage and intake air temperature.
 */
#include <math.h>
#include <stdio.h>
#include "ui_common.h"
#include "bike/bike.h"

#define T_MIN   40.0f
#define T_MAX   120.0f
// 240 degrees, not 270: the arcs end higher, leaving the bottom of the
// circle free for the readouts (at 270 the arc ends sat on top of them).
#define START   150
#define SWEEP   240

static lv_obj_t *s_arc, *s_value, *s_volt, *s_volt_state, *s_iat, *s_chart;
static lv_chart_series_t *s_series;
static smooth_t s_cool = {.tau = 0.6f}, s_volt_v = {.tau = 0.5f};
static uint32_t s_hist_seq = UINT32_MAX;
static lv_color_t s_arc_color;

static int angle_of(float c)
{
    return START + (int)(SWEEP * (c - T_MIN) / (T_MAX - T_MIN));
}

static void zone(lv_obj_t *tile, float from, float to, lv_color_t color)
{
    int a = angle_of(from), b = angle_of(to);
    lv_obj_t *z = ui_arc(tile, 344, 4, a, b - a, color, color);
    lv_obj_set_style_arc_rounded(z, false, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(z, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(z, LV_OPA_70, LV_PART_MAIN);
}

static lv_obj_t *stat(lv_obj_t *tile, int x, const char *caption, lv_obj_t **value)
{
    *value = ui_label(tile, &font_num_32, C_TEXT, "--");
    lv_obj_align(*value, LV_ALIGN_CENTER, x, 78);
    lv_obj_t *c = ui_label(tile, &font_txt_14, C_DIM, caption);
    lv_obj_align(c, LV_ALIGN_CENTER, x, 104);
    return c;
}

static void create(lv_obj_t *tile)
{
    zone(tile, T_MIN, BIKE_COOLANT_COLD, C_COLD);
    zone(tile, BIKE_COOLANT_COLD, BIKE_COOLANT_WARN, C_OK);
    zone(tile, BIKE_COOLANT_WARN, BIKE_COOLANT_ALERT, C_WARN);
    zone(tile, BIKE_COOLANT_ALERT, T_MAX, C_ALERT);
    s_arc = ui_arc(tile, 324, 16, START, SWEEP, C_TRACK, C_TEXT);
    s_arc_color = C_TEXT;

    lv_obj_t *cap = ui_label(tile, &font_txt_14, C_DIM, "COOLANT");
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -92);
    s_value = ui_label(tile, &font_num_80, C_TEXT, "--");
    lv_obj_align(s_value, LV_ALIGN_CENTER, 0, -38);

    s_chart = lv_chart_create(tile);
    lv_obj_set_size(s_chart, 170, 36);
    lv_obj_align(s_chart, LV_ALIGN_CENTER, 0, 22);
    lv_chart_set_type(s_chart, LV_CHART_TYPE_LINE);
    lv_chart_set_point_count(s_chart, COOLANT_HIST);
    lv_chart_set_axis_range(s_chart, LV_CHART_AXIS_PRIMARY_Y, 40, 110);
    lv_chart_set_div_line_count(s_chart, 0, 0);
    lv_chart_set_update_mode(s_chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_bg_opa(s_chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_chart, 0, 0);
    lv_obj_set_style_pad_all(s_chart, 0, 0);
    lv_obj_set_style_line_width(s_chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(s_chart, 0, LV_PART_INDICATOR);   // no point dots
    lv_obj_set_style_height(s_chart, 0, LV_PART_INDICATOR);
    lv_obj_remove_flag(s_chart, LV_OBJ_FLAG_CLICKABLE);
    s_series = lv_chart_add_series(s_chart, C_ACCENT, LV_CHART_AXIS_PRIMARY_Y);
    lv_chart_set_all_values(s_chart, s_series, LV_CHART_POINT_NONE);
    lv_obj_t *span = ui_label(tile, &font_txt_14, C_FAINT, "10 min");
    lv_obj_align_to(span, s_chart, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

    stat(tile, -62, "VOLTS", &s_volt);
    stat(tile, 62, "INTAKE AIR", &s_iat);
    s_volt_state = ui_label(tile, &font_txt_14, C_DIM, "");
    lv_obj_align(s_volt_state, LV_ALIGN_CENTER, -62, 122);
}

static void update(const dash_t *d, float dt, bool visible)
{
    float cool = 0, volts = 0, iat = 0, rpm = 0;
    bool has_cool = obd_value(&d->obd, PID_COOLANT, d->now, &cool);
    bool has_volt = obd_value(&d->obd, PID_VOLTAGE, d->now, &volts);
    bool has_iat = obd_value(&d->obd, PID_IAT, d->now, &iat);
    bool running = obd_value(&d->obd, PID_RPM, d->now, &rpm) && rpm > 800;
    smooth_set(&s_cool, has_cool, cool);
    smooth_set(&s_volt_v, has_volt, volts);
    float c = smooth_step(&s_cool, dt);
    float v = smooth_step(&s_volt_v, dt);
    if (!visible) {
        return;
    }

    char buf[24];
    ui_arc_set(s_arc, has_cool ? (int)((c - T_MIN) / (T_MAX - T_MIN) * 1000) : 0);
    lv_color_t col = has_cool ? ui_coolant_color(cool) : C_FAINT;
    if (!lv_color_eq(col, s_arc_color)) {
        s_arc_color = col;
        lv_obj_set_style_arc_color(s_arc, lv_color_eq(col, C_TEXT) ? C_TEXT : col, LV_PART_INDICATOR);
    }
    if (has_cool) {
        snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)roundf(c));
        ui_label_set(s_value, buf);
    } else {
        ui_label_set(s_value, "--");
    }
    ui_label_color(s_value, col);

    if (has_volt) {
        snprintf(buf, sizeof(buf), "%.1f", v);
        ui_label_set(s_volt, buf);
        bool bad = v < BIKE_VOLT_LOW && running ? true : v > BIKE_VOLT_HIGH;
        ui_label_color(s_volt, bad ? C_ALERT : C_TEXT);
        const char *state = !running ? "engine off" : v >= BIKE_VOLT_CHARGING ? "charging" : "not charging";
        ui_label_set(s_volt_state, state);
        ui_label_color(s_volt_state, running && v < BIKE_VOLT_CHARGING ? C_WARN : C_FAINT);
    } else {
        ui_label_set(s_volt, "--");
        ui_label_color(s_volt, C_FAINT);
        ui_label_set(s_volt_state, "");
    }
    if (has_iat) {
        snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)roundf(iat));
        ui_label_set(s_iat, buf);
        ui_label_color(s_iat, C_TEXT);
    } else {
        ui_label_set(s_iat, "--");
        ui_label_color(s_iat, C_FAINT);
    }

    if (d->coolant_hist_seq != s_hist_seq) {
        s_hist_seq = d->coolant_hist_seq;
        lv_chart_set_all_values(s_chart, s_series, LV_CHART_POINT_NONE);
        int off = COOLANT_HIST - d->coolant_hist_n;
        for (int i = 0; i < d->coolant_hist_n; i++) {
            lv_chart_set_series_value_by_id(s_chart, s_series, off + i, (int32_t)d->coolant_hist[i]);
        }
        lv_chart_refresh(s_chart);
    }
}

const ui_page_t PAGE_ENGINE = {create, update, NULL};
