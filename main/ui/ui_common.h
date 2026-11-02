/*
 * Shared look and motion for every page: the palette (black, with gold as
 * the accent), the fonts, label helpers, and the value
 * smoothing that makes needles and digits glide between OBD samples.
 */
#pragma once

#include <stdbool.h>
#include "lvgl.h"
#include "app.h"

#define UI_W 360
#define UI_H 360

// Palette
#define C_BG        lv_color_hex(0x000000)
#define C_TRACK     lv_color_hex(0x16191F)
#define C_TRACK_HI  lv_color_hex(0x262A33)
#define C_TEXT      lv_color_hex(0xF2F4F7)
#define C_DIM       lv_color_hex(0x7C8391)
#define C_FAINT     lv_color_hex(0x3A3F4A)
#define C_ACCENT    lv_color_hex(0xD4A64A)   // fork gold
#define C_COLD      lv_color_hex(0x3BA7FF)
#define C_OK        lv_color_hex(0x34D399)
#define C_WARN      lv_color_hex(0xF5A524)
#define C_ALERT     lv_color_hex(0xFF453A)
#define C_CYAN      lv_color_hex(0x5AC8FA)
#define C_VIOLET    lv_color_hex(0xA78BFA)

LV_FONT_DECLARE(font_num_120);
LV_FONT_DECLARE(font_num_80);
LV_FONT_DECLARE(font_num_48);
LV_FONT_DECLARE(font_num_32);
LV_FONT_DECLARE(font_txt_24);
LV_FONT_DECLARE(font_txt_18);
LV_FONT_DECLARE(font_txt_14);

// A value that eases toward its target: exponential smoothing with time
// constant tau, so motion is frame-rate independent.
typedef struct {
    float cur, target;
    float tau;          // s
    bool valid;         // the target is current
    bool init;
} smooth_t;

void smooth_set(smooth_t *s, bool valid, float target);
float smooth_step(smooth_t *s, float dt);

lv_obj_t *ui_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
// Change a label's text only when it differs (no needless redraw).
void ui_label_set(lv_obj_t *label, const char *text);
void ui_label_color(lv_obj_t *label, lv_color_t color);
// A display-only arc: no knob, no touch, rounded ends.
lv_obj_t *ui_arc(lv_obj_t *parent, int size, int width, int start_deg, int sweep_deg, lv_color_t track,
                 lv_color_t indicator);
void ui_arc_set(lv_obj_t *arc, int value);   // only when it changed
// A plain container: transparent, no padding, no scrolling, clicks pass through.
lv_obj_t *ui_box(lv_obj_t *parent, int w, int h);
// A centred column that scrolls vertically, clear of the rim at the top and the
// page segments at the bottom (Status, Rides, a sheet).
lv_obj_t *ui_column(lv_obj_t *parent);

// "5,841"
void ui_fmt_thousands(char *out, size_t n, int v);
// The colour for a coolant temperature.
lv_color_t ui_coolant_color(float c);

// Page lifecycle: create once in its tile, update every frame with the snapshot.
typedef struct {
    void (*create)(lv_obj_t *tile);
    void (*update)(const dash_t *d, float dt, bool visible);
    void (*hold)(void);   // the page's long-press action (touch or BOOT button), or NULL
} ui_page_t;

extern const ui_page_t PAGE_RIDE, PAGE_TYRES, PAGE_ENGINE, PAGE_PERF, PAGE_LEAN, PAGE_TRIP, PAGE_FUEL, PAGE_DIAG,
                        PAGE_RIDES, PAGE_STATUS;

// From pages to the shell.
void ui_toast(const char *text);
void ui_confirm(const char *title, const char *body, const char *ok_text, lv_color_t ok_color, void (*on_ok)(void));
// A modal with a 0..max_whole roller, a .0-.9 roller when tenths, the unit, and
// Cancel / Save; on_ok gets the value.
void ui_number_picker(const char *title, const char *body, int max_whole, bool tenths, const char *unit, float initial,
                      void (*on_ok)(float));
void ui_tick_feedback(void);
// A full-screen view over the pages; returns a ui_column() to fill. It counts
// as a dialog: a BOOT press closes it, like ui_sheet_close().
lv_obj_t *ui_sheet(void);
void ui_sheet_close(void);
// A full-screen page opened from another page (Status opens Rides and
// Diagnostics): built once at boot, updated every frame, shown over the pager.
// A BOOT press, a swipe right or ui_view_close() goes back.
void ui_view_open(const ui_page_t *page);
void ui_view_close(void);
// A view's caption with a back arrow: tapping it closes the view.
lv_obj_t *ui_back_caption(lv_obj_t *parent, lv_color_t color, const char *text);
