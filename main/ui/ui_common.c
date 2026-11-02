#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ui_common.h"
#include "bike/bike.h"

void smooth_set(smooth_t *s, bool valid, float target)
{
    s->valid = valid;
    if (!valid) {
        return;
    }
    if (!s->init) {
        s->cur = target;
        s->init = true;
    }
    s->target = target;
}

float smooth_step(smooth_t *s, float dt)
{
    if (s->init) {
        float k = 1.0f - expf(-dt / (s->tau > 0 ? s->tau : 0.15f));
        s->cur += (s->target - s->cur) * k;
        if (fabsf(s->target - s->cur) < 1e-3f) s->cur = s->target;
    }
    return s->cur;
}

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_label_set_text(l, text);
    return l;
}

void ui_label_set(lv_obj_t *label, const char *text)
{
    const char *cur = lv_label_get_text(label);
    if (!cur || strcmp(cur, text) != 0) {
        lv_label_set_text(label, text);
    }
}

void ui_label_color(lv_obj_t *label, lv_color_t color)
{
    if (!lv_color_eq(lv_obj_get_style_text_color(label, 0), color)) {
        lv_obj_set_style_text_color(label, color, 0);
    }
}

lv_obj_t *ui_arc(lv_obj_t *parent, int size, int width, int start_deg, int sweep_deg, lv_color_t track,
                 lv_color_t indicator)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, size, size);
    lv_arc_set_bg_angles(a, start_deg, start_deg + sweep_deg);
    lv_arc_set_rotation(a, 0);
    lv_arc_set_range(a, 0, 1000);
    lv_arc_set_value(a, 0);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(a, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, track, LV_PART_MAIN);
    lv_obj_set_style_arc_color(a, indicator, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_pad_all(a, 0, 0);
    lv_obj_center(a);
    return a;
}

void ui_arc_set(lv_obj_t *arc, int value)
{
    if (value < 0) value = 0;
    if (value > 1000) value = 1000;
    if (lv_arc_get_value(arc) != value) {
        lv_arc_set_value(arc, value);
    }
}

lv_obj_t *ui_box(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *ui_column(lv_obj_t *parent)
{
    lv_obj_t *col = lv_obj_create(parent);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, 260, UI_H);
    lv_obj_center(col);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(col, 56, 0);
    lv_obj_set_style_pad_bottom(col, 90, 0);
    lv_obj_set_style_pad_row(col, 8, 0);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(col, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    return col;
}

static void on_back(lv_event_t *e)
{
    ui_tick_feedback();
    ui_view_close();
}

lv_obj_t *ui_back_caption(lv_obj_t *parent, lv_color_t color, const char *text)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(btn, 14, 0);
    lv_obj_set_style_pad_ver(btn, 6, 0);
    lv_obj_set_style_radius(btn, 14, 0);
    lv_obj_set_style_bg_color(btn, C_TRACK_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn, 8, 0);
    lv_obj_add_event_cb(btn, on_back, LV_EVENT_CLICKED, NULL);
    ui_label(btn, &lv_font_montserrat_14, color, LV_SYMBOL_LEFT);
    lv_obj_t *l = ui_label(btn, &font_txt_14, color, text);
    lv_obj_set_style_text_letter_space(l, 3, 0);
    return btn;
}

void ui_fmt_thousands(char *out, size_t n, int v)
{
    if (v >= 1000) {
        snprintf(out, n, "%d,%03d", v / 1000, v % 1000);
    } else {
        snprintf(out, n, "%d", v);
    }
}

lv_color_t ui_coolant_color(float c)
{
    if (c >= BIKE_COOLANT_ALERT) return C_ALERT;
    if (c >= BIKE_COOLANT_WARN) return C_WARN;
    if (c < BIKE_COOLANT_COLD) return C_COLD;
    return C_TEXT;
}
