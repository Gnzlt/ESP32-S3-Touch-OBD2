/*
 * Diagnostics, opened from the Status page: the MIL, stored and pending fault
 * codes with a short description each, distance with the MIL on, readiness
 * monitors, and Clear codes (Mode 04) behind a confirm, allowed only with the
 * engine stopped.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ui_common.h"
#include "obd/dtc.h"
#include "obd/pids.h"

static lv_obj_t *s_mil, *s_mil_dot, *s_list, *s_info, *s_btn, *s_btn_label;
static uint32_t s_list_hash = 1;
static clear_state_t s_last_clear = CLEAR_NONE;
static bool s_engine_running, s_live;

static void do_clear(void)
{
    app_request(REQ_CLEAR_CODES);
}

static void on_clear(lv_event_t *e)
{
    ui_tick_feedback();
    if (!s_live) {
        ui_toast("No connection to the ECU");
        return;
    }
    if (s_engine_running) {
        ui_toast("Stop the engine first");
        return;
    }
    ui_confirm("Clear fault codes?",
               "Ignition on, engine off.\nAlso erases freeze-frame data\nand readiness monitors.",
               "Clear", C_ALERT, do_clear);
}

static void create(lv_obj_t *tile)
{
    lv_obj_t *cap = ui_back_caption(tile, C_DIM, "DIAGNOSTICS");
    lv_obj_align(cap, LV_ALIGN_CENTER, 0, -134);

    lv_obj_t *pill = ui_box(tile, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pill, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pill, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(pill, 8, 0);
    lv_obj_align(pill, LV_ALIGN_CENTER, 0, -104);
    s_mil_dot = ui_box(pill, 12, 12);
    lv_obj_set_style_radius(s_mil_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_mil_dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_mil_dot, C_FAINT, 0);
    s_mil = ui_label(pill, &font_txt_18, C_DIM, "MIL --");

    // The code list scrolls vertically if there are many.
    s_list = lv_obj_create(tile);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_size(s_list, 250, 150);
    lv_obj_align(s_list, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    s_info = ui_label(tile, &font_txt_14, C_DIM, "");
    lv_obj_set_style_text_align(s_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_info, LV_ALIGN_CENTER, 0, 88);

    s_btn = lv_button_create(tile);
    lv_obj_remove_style_all(s_btn);
    lv_obj_set_size(s_btn, 150, 40);
    lv_obj_align(s_btn, LV_ALIGN_CENTER, 0, 128);
    lv_obj_set_style_radius(s_btn, 20, 0);
    lv_obj_set_style_border_width(s_btn, 2, 0);
    lv_obj_set_style_border_color(s_btn, C_ALERT, 0);
    lv_obj_set_style_bg_color(s_btn, C_ALERT, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(s_btn, LV_OPA_40, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_btn, on_clear, LV_EVENT_CLICKED, NULL);
    s_btn_label = ui_label(s_btn, &font_txt_18, C_ALERT, "Clear codes");
    lv_obj_center(s_btn_label);
}

static void add_code(const char *code, bool pending)
{
    lv_obj_t *row = ui_box(s_list, 250, LV_SIZE_CONTENT);
    lv_obj_t *c = ui_label(row, &font_num_32, pending ? C_WARN : C_ALERT, code);
    lv_obj_align(c, LV_ALIGN_TOP_MID, 0, 0);
    char text[64];
    snprintf(text, sizeof(text), "%s%s", pending ? "Pending: " : "", dtc_describe(code));
    lv_obj_t *t = ui_label(row, &font_txt_14, C_DIM, text);
    lv_obj_set_width(t, 240);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 34);
}

// PID 41 readiness: monitors available vs still incomplete this drive cycle.
static void monitors(const uint8_t *r, int *ready, int *avail)
{
    int a = 0, inc = 0;
    for (int i = 0; i < 3; i++) {
        if (r[1] & (1 << i)) {
            a++;
            if (r[1] & (1 << (i + 4))) inc++;
        }
    }
    for (int i = 0; i < 8; i++) {
        if (r[2] & (1 << i)) {
            a++;
            if (r[3] & (1 << i)) inc++;
        }
    }
    *avail = a;
    *ready = a - inc;
}

static void update(const dash_t *d, float dt, bool visible)
{
    const obd_state_t *o = &d->obd;
    float rpm = 0;
    s_engine_running = obd_value(o, PID_RPM, d->now, &rpm) && rpm >= 300;
    s_live = o->phase == PHASE_LIVE;

    if (o->clear != s_last_clear) {
        s_last_clear = o->clear;
        if (o->clear == CLEAR_OK) ui_toast("Fault codes cleared");
        if (o->clear == CLEAR_FAILED) ui_toast("The ECU refused to clear");
    }
    if (!visible) {
        return;
    }

    lv_obj_set_style_bg_color(s_mil_dot, !s_live ? C_FAINT : o->mil ? C_WARN : C_OK, 0);
    ui_label_set(s_mil, !s_live ? "MIL --" : o->mil ? "MIL ON" : "MIL OFF");
    ui_label_color(s_mil, !s_live ? C_FAINT : o->mil ? C_WARN : C_TEXT);

    uint32_t h = (uint32_t)o->n_dtcs * 97u + (uint32_t)o->n_pending * 13u + (o->dtcs_read ? 1u : 0u) + (s_live ? 7u : 0u);
    for (int i = 0; i < o->n_dtcs; i++) h = h * 31u + (uint32_t)o->dtcs[i][1] * 7u + (uint32_t)o->dtcs[i][4];
    for (int i = 0; i < o->n_pending; i++) h = h * 37u + (uint32_t)o->pending[i][4];
    if (h != s_list_hash) {
        s_list_hash = h;
        lv_obj_clean(s_list);
        if (o->n_dtcs == 0 && o->n_pending == 0) {
            lv_obj_t *none = ui_label(s_list, &font_txt_18, s_live && o->dtcs_read ? C_OK : C_FAINT,
                                      s_live && o->dtcs_read ? "No fault codes" : "Codes not read yet");
            (void)none;
        }
        for (int i = 0; i < o->n_dtcs; i++) add_code(o->dtcs[i], false);
        for (int i = 0; i < o->n_pending; i++) {
            bool dup = false;
            for (int k = 0; k < o->n_dtcs; k++) dup |= strcmp(o->dtcs[k], o->pending[i]) == 0;
            if (!dup) add_code(o->pending[i], true);
        }
    }

    char info[96];
    int n = 0;
    float v;
    if (obd_value(o, PID_MIL_DIST, d->now, &v)) {
        n += snprintf(info + n, sizeof(info) - n, "MIL on for %d km", (int)v);
    }
    if (obd_value(o, PID_MONITOR, d->now, &v)) {
        int ready, avail;
        monitors(o->raw[PID_MONITOR], &ready, &avail);
        n += snprintf(info + n, sizeof(info) - n, "%sMonitors %d/%d ready", n ? "  \xE2\x80\x93  " : "", ready, avail);
    }
    if (obd_value(o, PID_OBD_STD, d->now, &v)) {
        const char *std = pid_obd_std_text(o->raw[PID_OBD_STD][0]);
        if (std) {
            n += snprintf(info + n, sizeof(info) - n, "\n%s", std);
        } else {
            n += snprintf(info + n, sizeof(info) - n, "\nOBD standard code %d", o->raw[PID_OBD_STD][0]);
        }
    }
    info[n] = 0;
    ui_label_set(s_info, info);

    bool can = s_live && !s_engine_running;
    lv_obj_set_style_border_color(s_btn, can ? C_ALERT : C_FAINT, 0);
    ui_label_color(s_btn_label, can ? C_ALERT : C_FAINT);
    ui_label_set(s_btn_label, o->clear == CLEAR_PENDING || o->clear == CLEAR_SENT ? "Clearing..."
                              : s_engine_running ? "Engine running" : "Clear codes");
}

const ui_page_t PAGE_DIAG = {create, update, NULL};
