/*
 * Page 7, Fuel (estimated): range and fuel left since the last fill-up, the
 * consumption since then and this trip, from the load-based estimate in
 * obd/fuel.c times the calibration factor k. Labelled "est." throughout, and
 * "uncalibrated" while k is still 1. "Filled up" asks for the litres pumped:
 * each fill-up is a calibration point in fillups.csv on the card.
 */
#include <math.h>
#include <stdio.h>
#include "ui_common.h"
#include "bike/bike.h"

#define RANGE_MIN_KM 5.0    // below this since the fill-up, the average is too young to project

typedef enum { F_LEFT, F_AVG, F_TRIP, F_USED, F_COUNT } fuel_id_t;

static lv_obj_t *s_range, *s_range_cap, *s_val[F_COUNT], *s_hint;
static uint32_t s_last_hash = 1;

static void on_litres(float litres)
{
    if (litres < 0.1f) {
        ui_toast("No litres: not saved");
        return;
    }
    app_request_fill(litres);
    char buf[32];
    snprintf(buf, sizeof(buf), "Fill-up saved: %.1f L", litres);
    ui_toast(buf);
}

static void on_filled(lv_event_t *e)
{
    ui_tick_feedback();
    ui_number_picker("Filled up", "Litres pumped (fill to the brim)", 25, true, "L", 10.0f, on_litres);
}

static void cell(lv_obj_t *tile, fuel_id_t id, int x, int y, const char *caption)
{
    s_val[id] = ui_label(tile, &font_num_32, C_TEXT, "--");
    lv_obj_align(s_val[id], LV_ALIGN_CENTER, x, y);
    lv_obj_t *c = ui_label(tile, &font_txt_14, C_DIM, caption);
    lv_obj_align(c, LV_ALIGN_CENTER, x, y + 24);
}

static void create(lv_obj_t *tile)
{
    lv_obj_t *cap = ui_label(tile, &font_txt_14, C_DIM, "FUEL  EST.");
    lv_obj_set_style_text_letter_space(cap, 3, 0);
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -128);
    s_range = ui_label(tile, &font_num_80, C_ACCENT, "--");
    lv_obj_align(s_range, LV_ALIGN_CENTER, 0, -84);
    s_range_cap = ui_label(tile, &font_txt_14, C_DIM, "km range");
    lv_obj_align(s_range_cap, LV_ALIGN_CENTER, 0, -42);

    cell(tile, F_LEFT, -64, -14, "LEFT L");
    cell(tile, F_AVG, 64, -14, "L/100 FILL");
    cell(tile, F_TRIP, -64, 42, "L/100 TRIP");
    cell(tile, F_USED, 64, 42, "USED L");

    s_hint = ui_label(tile, &font_txt_14, C_WARN, "");
    lv_obj_align(s_hint, LV_ALIGN_CENTER, 0, 88);

    lv_obj_t *btn = lv_button_create(tile);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 150, 40);
    lv_obj_align(btn, LV_ALIGN_CENTER, 0, 122);
    lv_obj_set_style_radius(btn, 20, 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_border_color(btn, C_ACCENT, 0);
    lv_obj_set_style_bg_color(btn, C_ACCENT, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_add_event_cb(btn, on_filled, LV_EVENT_CLICKED, NULL);
    lv_obj_t *bl = ui_label(btn, &font_txt_18, C_ACCENT, "Filled up");
    lv_obj_center(bl);
}

static void set_l100(lv_obj_t *label, double litres, double km)
{
    char buf[16];
    if (km >= 1.0) {
        snprintf(buf, sizeof(buf), "%.1f", litres / km * 100.0);
        ui_label_set(label, buf);
    } else {
        ui_label_set(label, "--");
    }
}

static void update(const dash_t *d, float dt, bool visible)
{
    if (!visible) {
        return;
    }
    const fill_t *f = &d->fill;
    const trip_t *t = &d->trip;
    uint32_t h = (uint32_t)(f->km * 10) * 31u + (uint32_t)(f->raw_ml / 10) * 7u + f->fills * 131u
                 + (uint32_t)(t->fuel_raw_ml / 10) * 13u + (uint32_t)(t->distance_m / 100) + (uint32_t)(f->k * 1000);
    if (h == s_last_hash) {
        return;
    }
    s_last_hash = h;

    double used = f->raw_ml / 1000.0 * f->k;
    char buf[24];
    snprintf(buf, sizeof(buf), "%.1f", used);
    ui_label_set(s_val[F_USED], buf);
    set_l100(s_val[F_AVG], used, f->km);
    set_l100(s_val[F_TRIP], t->fuel_raw_ml / 1000.0 * f->k, t->distance_m / 1000.0);

    if (f->fills == 0) {
        // No fill-up marked yet: nothing to count down from.
        ui_label_set(s_range, "--");
        ui_label_set(s_val[F_LEFT], "--");
        ui_label_set(s_hint, "Tap Filled up at your next fill");
        return;
    }
    double left = BIKE_TANK_L - used;
    if (left < 0) left = 0;
    snprintf(buf, sizeof(buf), "%.1f", left);
    ui_label_set(s_val[F_LEFT], buf);
    ui_label_color(s_val[F_LEFT], left < 3.0 ? C_WARN : C_TEXT);
    if (f->km >= RANGE_MIN_KM && used > 0) {
        snprintf(buf, sizeof(buf), "%d", (int)(left / (used / f->km)));
        ui_label_set(s_range, buf);
        ui_label_color(s_range, left < 3.0 ? C_WARN : C_ACCENT);
    } else {
        ui_label_set(s_range, "--");
    }
    ui_label_set(s_hint, f->k == 1.0f ? "uncalibrated" : "");
}

const ui_page_t PAGE_FUEL = {create, update, NULL};
