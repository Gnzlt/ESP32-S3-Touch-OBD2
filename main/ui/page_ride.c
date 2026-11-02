/*
 * Page 1, Ride: the glance page. RPM sweeps around the rim with a red zone,
 * speed in the middle in the biggest digits, throttle as a thin inner arc,
 * and the estimated gear in a badge at the bottom (hidden when the bike
 * profile gives no gear ratios). The rev counter runs to the profile's
 * BIKE_RPM_MAX, labelled in thousands.
 */
#include <math.h>
#include <stdio.h>
#include "ui_common.h"
#include "bike/bike.h"
#include "obd/gear.h"

static lv_obj_t *s_rpm_arc, *s_thr_arc, *s_speed, *s_unit, *s_rpm, *s_gear, *s_gear_box;
static smooth_t s_rpm_v = {.tau = 0.08f}, s_speed_v = {.tau = 0.25f}, s_thr_v = {.tau = 0.12f};
static int s_zone = -1;

#define START 135
#define SWEEP 270
#define RPM_K ((int)(BIKE_RPM_MAX / 1000))   // the scale's labels, in thousands

static void create(lv_obj_t *tile)
{
    // Red zone under the track, so it shows before the needle gets there.
    int red_from = START + (int)(SWEEP * BIKE_RPM_RED / BIKE_RPM_MAX);
    lv_obj_t *red = ui_arc(tile, 344, 14, red_from, START + SWEEP - red_from, lv_color_hex(0x3A1212), C_ALERT);
    lv_obj_set_style_arc_opa(red, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(red, false, LV_PART_MAIN);

    s_rpm_arc = ui_arc(tile, 344, 14, START, SWEEP, C_TRACK, C_TEXT);
    lv_obj_set_style_arc_opa(s_rpm_arc, LV_OPA_60, LV_PART_MAIN);

    lv_obj_t *scale = lv_scale_create(tile);
    lv_obj_set_size(scale, 312, 312);
    lv_obj_center(scale);
    lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(scale, 0, RPM_K);
    lv_scale_set_total_tick_count(scale, RPM_K * 2 + 1);
    lv_scale_set_major_tick_every(scale, 2);
    lv_scale_set_angle_range(scale, SWEEP);
    lv_scale_set_rotation(scale, START);
    lv_scale_set_label_show(scale, true);
    lv_obj_set_style_bg_opa(scale, LV_OPA_TRANSP, 0);
    lv_obj_set_style_length(scale, 9, LV_PART_INDICATOR);
    lv_obj_set_style_length(scale, 5, LV_PART_ITEMS);
    lv_obj_set_style_line_width(scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(scale, 1, LV_PART_ITEMS);
    lv_obj_set_style_line_color(scale, C_DIM, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(scale, C_FAINT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(scale, &font_txt_14, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(scale, C_DIM, LV_PART_INDICATOR);
    lv_obj_set_style_pad_radial(scale, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(scale, 0, LV_PART_MAIN);
    lv_scale_section_t *sec = lv_scale_add_section(scale);
    lv_scale_set_section_range(scale, sec, (int)(BIKE_RPM_RED / 1000), RPM_K);
    static lv_style_t red_ticks;
    lv_style_init(&red_ticks);
    lv_style_set_line_color(&red_ticks, C_ALERT);
    lv_style_set_text_color(&red_ticks, C_ALERT);
    lv_scale_set_section_style_indicator(scale, sec, &red_ticks);
    lv_scale_set_section_style_items(scale, sec, &red_ticks);

    s_thr_arc = ui_arc(tile, 228, 4, START, SWEEP, C_TRACK, C_ACCENT);

    s_rpm = ui_label(tile, &font_num_32, C_DIM, "--");
    lv_obj_align(s_rpm, LV_ALIGN_CENTER, 0, -70);
    lv_obj_t *rpm_unit = ui_label(tile, &font_txt_14, C_FAINT, "RPM");
    lv_obj_align(rpm_unit, LV_ALIGN_CENTER, 0, -46);

    s_speed = ui_label(tile, &font_num_120, C_TEXT, "--");
    lv_obj_align(s_speed, LV_ALIGN_CENTER, 0, 10);
    s_unit = ui_label(tile, &font_txt_14, C_DIM, "km/h");
    lv_obj_align(s_unit, LV_ALIGN_CENTER, 0, 68);

    s_gear_box = lv_obj_create(tile);
    lv_obj_remove_style_all(s_gear_box);
    lv_obj_set_size(s_gear_box, 50, 50);
    lv_obj_align(s_gear_box, LV_ALIGN_CENTER, 0, 116);
    lv_obj_set_style_radius(s_gear_box, 14, 0);
    lv_obj_set_style_border_width(s_gear_box, 2, 0);
    lv_obj_set_style_border_color(s_gear_box, C_FAINT, 0);
    lv_obj_remove_flag(s_gear_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    // The digit's line box is taller than the badge: let it draw past the edge
    // rather than be clipped (a clipped label leaves stale pixels when it changes).
    lv_obj_add_flag(s_gear_box, LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    s_gear = ui_label(s_gear_box, &font_num_48, C_ACCENT, "-");
    lv_obj_center(s_gear);
    if (gear_count() == 0) {
        lv_obj_add_flag(s_gear_box, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update(const dash_t *d, float dt, bool visible)
{
    float rpm = 0, kmh = 0, thr = 0;
    bool has_rpm = obd_value(&d->obd, PID_RPM, d->now, &rpm);
    bool has_kmh = obd_value(&d->obd, PID_SPEED, d->now, &kmh);
    bool has_thr = obd_value(&d->obd, PID_THROTTLE, d->now, &thr);
    // Stale values fall to zero on the gauges, so a dead link reads as dead.
    smooth_set(&s_rpm_v, true, has_rpm ? rpm : 0);
    smooth_set(&s_speed_v, true, has_kmh ? kmh : 0);
    smooth_set(&s_thr_v, true, has_thr ? thr : 0);
    float r = smooth_step(&s_rpm_v, dt);
    float v = smooth_step(&s_speed_v, dt);
    float t = smooth_step(&s_thr_v, dt);
    if (!visible) {
        return;
    }
    ui_arc_set(s_rpm_arc, (int)(r / BIKE_RPM_MAX * 1000));
    ui_arc_set(s_thr_arc, (int)(t * 10));
    int zone = r >= BIKE_RPM_RED ? 2 : r >= BIKE_RPM_RED - 1000 ? 1 : 0;
    if (zone != s_zone) {
        s_zone = zone;
        lv_obj_set_style_arc_color(s_rpm_arc, zone == 2 ? C_ALERT : zone == 1 ? C_WARN : C_TEXT, LV_PART_INDICATOR);
    }

    char buf[16];
    if (has_rpm) {
        ui_fmt_thousands(buf, sizeof(buf), (int)(roundf(r / 10) * 10));
        ui_label_set(s_rpm, buf);
        ui_label_color(s_rpm, zone == 2 ? C_ALERT : C_DIM);
    } else {
        ui_label_set(s_rpm, "--");
        ui_label_color(s_rpm, C_FAINT);
    }
    if (has_kmh) {
        snprintf(buf, sizeof(buf), "%d", (int)roundf(v));
        ui_label_set(s_speed, buf);
        ui_label_color(s_speed, C_TEXT);
    } else {
        ui_label_set(s_speed, "--");
        ui_label_color(s_speed, C_FAINT);
    }
    if (d->gear > 0) {
        snprintf(buf, sizeof(buf), "%d", d->gear);
        ui_label_set(s_gear, buf);
        ui_label_color(s_gear, C_ACCENT);
    } else {
        // Stopped, a shift or the clutch in: OBD can't tell neutral from first
        // with the clutch pulled, so no guess.
        ui_label_set(s_gear, "-");
        ui_label_color(s_gear, C_FAINT);
    }
}

const ui_page_t PAGE_RIDE = {create, update, NULL};
