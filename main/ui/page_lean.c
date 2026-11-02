/*
 * Page 5, Lean & G: a lean scale across the top with a needle, the lean in
 * big digits, the ride's maximum each way, and a G-ball with a fading trail.
 * Lean and G only show while riding (from 15 km/h): standing still they read
 * "--". The bike's level is learned while riding; a long-press (bike upright,
 * standing still) only gives it a head start.
 */
#include <math.h>
#include <stdio.h>
#include "ui_common.h"

#define LEAN_MAX   60
#define BALL_R     44     // 1 G at the rim
#define TRAIL      8

static lv_obj_t *s_scale, *s_needle, *s_value, *s_side, *s_max_l, *s_max_r, *s_ball, *s_hint;
static lv_obj_t *s_trail[TRAIL];
static lv_point_precise_t s_needle_pts[2];
// Shown as an average over about half a second: lean and G change no faster on
// a bike, and the estimate's own jitter (several degrees when slowing to a
// stop) stays off the screen. The peaks and trip.csv use the unsmoothed value.
static smooth_t s_lean = {.tau = 0.5f}, s_lat = {.tau = 0.4f}, s_lon = {.tau = 0.4f};
static float s_trail_x[TRAIL], s_trail_y[TRAIL];
static float s_trail_t;
#define NOT_RIDING 1000
static int s_last_deg = -NOT_RIDING;   // neither a lean nor "not riding": the first frame sets the label

static void hold(void)
{
    app_request(REQ_ZERO_LEAN);
    ui_tick_feedback();
    ui_toast("Lean zeroed");
}

static void on_long_press(lv_event_t *e) { hold(); }

static lv_obj_t *dot(lv_obj_t *parent, int size, lv_color_t color, lv_opa_t opa)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, size, size);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(o, color, 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void create(lv_obj_t *tile)
{
    lv_obj_add_event_cb(tile, on_long_press, LV_EVENT_LONG_PRESSED, NULL);

    // Lean scale: -60 .. +60 across the top, 0 straight up.
    s_scale = lv_scale_create(tile);
    lv_obj_set_size(s_scale, 330, 330);
    lv_obj_center(s_scale);
    lv_scale_set_mode(s_scale, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_range(s_scale, -LEAN_MAX, LEAN_MAX);
    lv_scale_set_total_tick_count(s_scale, 25);
    lv_scale_set_major_tick_every(s_scale, 4);
    lv_scale_set_angle_range(s_scale, 2 * LEAN_MAX);
    lv_scale_set_rotation(s_scale, 270 - LEAN_MAX);
    lv_scale_set_label_show(s_scale, true);
    static const char *labels[] = {"60", "40", "20", "0", "20", "40", "60", NULL};
    lv_scale_set_text_src(s_scale, labels);
    lv_obj_set_style_length(s_scale, 12, LV_PART_INDICATOR);
    lv_obj_set_style_length(s_scale, 6, LV_PART_ITEMS);
    lv_obj_set_style_line_width(s_scale, 2, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s_scale, C_DIM, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s_scale, C_FAINT, LV_PART_ITEMS);
    lv_obj_set_style_text_font(s_scale, &font_txt_14, LV_PART_INDICATOR);
    lv_obj_set_style_text_color(s_scale, C_DIM, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(s_scale, 0, LV_PART_MAIN);
    lv_obj_remove_flag(s_scale, LV_OBJ_FLAG_CLICKABLE);

    // A short needle near the rim, so it never crosses the digits.
    s_needle = lv_line_create(tile);
    lv_obj_set_style_line_width(s_needle, 5, 0);
    lv_obj_set_style_line_rounded(s_needle, true, 0);
    lv_obj_set_style_line_color(s_needle, C_ACCENT, 0);

    lv_obj_t *cap = ui_label(tile, &font_txt_14, C_DIM, "LEAN");
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -70);
    s_value = ui_label(tile, &font_num_80, C_TEXT, "--");
    lv_obj_align(s_value, LV_ALIGN_CENTER, 0, -20);
    s_side = ui_label(tile, &font_txt_14, C_DIM, "");
    lv_obj_align(s_side, LV_ALIGN_CENTER, 0, 26);

    s_max_l = ui_label(tile, &font_num_32, C_TEXT, "0\xC2\xB0");
    lv_obj_align(s_max_l, LV_ALIGN_CENTER, -100, 52);
    lv_obj_t *cl = ui_label(tile, &font_txt_14, C_DIM, "MAX L");
    lv_obj_align(cl, LV_ALIGN_CENTER, -100, 78);
    s_max_r = ui_label(tile, &font_num_32, C_TEXT, "0\xC2\xB0");
    lv_obj_align(s_max_r, LV_ALIGN_CENTER, 100, 52);
    lv_obj_t *cr = ui_label(tile, &font_txt_14, C_DIM, "MAX R");
    lv_obj_align(cr, LV_ALIGN_CENTER, 100, 78);

    // G-ball
    lv_obj_t *ring = lv_obj_create(tile);
    lv_obj_remove_style_all(ring);
    lv_obj_set_size(ring, BALL_R * 2, BALL_R * 2);
    lv_obj_align(ring, LV_ALIGN_CENTER, 0, 96);
    lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(ring, 1, 0);
    lv_obj_set_style_border_color(ring, C_FAINT, 0);
    lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *inner = lv_obj_create(ring);
    lv_obj_remove_style_all(inner);
    lv_obj_set_size(inner, BALL_R, BALL_R);
    lv_obj_center(inner);
    lv_obj_set_style_radius(inner, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(inner, 1, 0);
    lv_obj_set_style_border_color(inner, C_TRACK_HI, 0);
    for (int i = 0; i < TRAIL; i++) {
        s_trail[i] = dot(ring, 6, C_ACCENT, (lv_opa_t)(20 + 12 * i));
        lv_obj_center(s_trail[i]);
    }
    s_ball = dot(ring, 12, C_TEXT, LV_OPA_COVER);
    lv_obj_center(s_ball);

    s_hint = ui_label(tile, &font_txt_14, C_DIM, "shows above 15 km/h");
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 150);
}

static void update(const dash_t *d, float dt, bool visible)
{
    const motion_t *m = &d->motion;
    // Not riding: everything eases back to centre.
    smooth_set(&s_lean, m->ok, m->valid ? m->lean : 0);
    smooth_set(&s_lat, m->ok, m->valid ? m->lat_g : 0);
    smooth_set(&s_lon, m->ok, m->valid ? m->lon_g : 0);
    float lean = smooth_step(&s_lean, dt), lat = smooth_step(&s_lat, dt), lon = smooth_step(&s_lon, dt);
    if (!visible) {
        return;
    }
    if (!m->ok) {
        ui_label_set(s_value, "--");
        ui_label_set(s_side, "IMU not answering");
        return;
    }
    int deg = m->valid ? (int)roundf(lean) : NOT_RIDING;
    if (deg != s_last_deg && deg == NOT_RIDING) {
        s_last_deg = deg;
        ui_label_set(s_value, "--");
        ui_label_set(s_side, "");
    } else if (deg != s_last_deg) {
        s_last_deg = deg;
        char buf[16];
        snprintf(buf, sizeof(buf), "%d\xC2\xB0", abs(deg));
        ui_label_set(s_value, buf);
        ui_label_set(s_side, deg <= -1 ? "LEFT" : deg >= 1 ? "RIGHT" : "UPRIGHT");
    }
    float clamped = lean < -LEAN_MAX ? -LEAN_MAX : lean > LEAN_MAX ? LEAN_MAX : lean;
    // The line object only spans the needle, so a move redraws a small area.
    float th = (270.0f + clamped) * (float)M_PI / 180.0f;
    float x0 = UI_W / 2 + 112 * cosf(th), y0 = UI_H / 2 + 112 * sinf(th);
    float x1 = UI_W / 2 + 150 * cosf(th), y1 = UI_H / 2 + 150 * sinf(th);
    int left = (int)fminf(x0, x1) - 4, top = (int)fminf(y0, y1) - 4;
    s_needle_pts[0].x = x0 - left;
    s_needle_pts[0].y = y0 - top;
    s_needle_pts[1].x = x1 - left;
    s_needle_pts[1].y = y1 - top;
    lv_obj_set_pos(s_needle, left, top);
    lv_obj_set_size(s_needle, (int)fabsf(x1 - x0) + 9, (int)fabsf(y1 - y0) + 9);
    lv_line_set_points(s_needle, s_needle_pts, 2);
    lv_obj_set_style_line_color(s_needle, fabsf(lean) >= 45 ? C_WARN : C_ACCENT, 0);

    char buf[16];
    snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)roundf(d->trip.max_lean_left));
    ui_label_set(s_max_l, buf);
    snprintf(buf, sizeof(buf), "%d\xC2\xB0", (int)roundf(d->trip.max_lean_right));
    ui_label_set(s_max_r, buf);

    // The ball: right = +x, accelerating = up the screen.
    float bx = lat * BALL_R, by = -lon * BALL_R;
    float r = sqrtf(bx * bx + by * by);
    if (r > BALL_R - 6) {
        bx *= (BALL_R - 6) / r;
        by *= (BALL_R - 6) / r;
    }
    lv_obj_align(s_ball, LV_ALIGN_CENTER, (int)bx, (int)by);
    s_trail_t += dt;
    if (s_trail_t >= 0.1f) {
        s_trail_t = 0;
        for (int i = 0; i < TRAIL - 1; i++) {
            s_trail_x[i] = s_trail_x[i + 1];
            s_trail_y[i] = s_trail_y[i + 1];
            lv_obj_align(s_trail[i], LV_ALIGN_CENTER, (int)s_trail_x[i], (int)s_trail_y[i]);
        }
        s_trail_x[TRAIL - 1] = bx;
        s_trail_y[TRAIL - 1] = by;
        lv_obj_align(s_trail[TRAIL - 1], LV_ALIGN_CENTER, (int)bx, (int)by);
    }
    if (m->valid) {
        lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    }
}

const ui_page_t PAGE_LEAN = {create, update, hold};
