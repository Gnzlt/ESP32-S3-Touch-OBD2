/*
 * The OBD task: owns the ELM327 session (or the demo feed), turns BLE events
 * into session calls, ticks it every 20 ms, and publishes its state for the
 * UI. Nothing else touches the session, so it needs no locking of its own.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "app.h"
#include "ble/obd_link.h"
#include "demo/demo.h"
#include "ble/ble_scan.h"
#include "tpms_service.h"
#include "elm/elm327.h"
#include "elm/elm_reply.h"
#include "obd_task.h"
#include "sd_log.h"

static const char *TAG = "obd";

#define TICK_MS        20
#define DEMO_PERIOD_MS 50

static QueueHandle_t s_events;
static EXT_RAM_BSS_ATTR obd_state_t s_state;
static elm327_t s_elm;
static elm_assembler_t s_asm;
static bool s_demo;
static bool s_ble_started;

// ---- Transport for the session ----------------------------------------------

static bool tr_send(void *ctx, const uint8_t *data, int len)
{
    sd_log_raw('>', data, len, app_now_ms());
    return obd_link_send(data, len);
}
static void tr_restart(void *ctx) { obd_link_restart(); }
static void tr_discard(void *ctx) { elm_assembler_reset(&s_asm); }
static void tr_save_protocol(void *ctx, const char *digit) { nvs_set_blob_("proto", digit, 2); }

static void on_reply(void *ctx, const uint8_t *reply, int len)
{
    sd_log_raw('<', reply, len, app_now_ms());
    elm_on_reply(&s_elm, reply, len, app_now_ms());
}

static void start_session(void)
{
    char proto[2] = "";
    if (!nvs_get_blob_("proto", proto, 2) || proto[1] != 0) {
        proto[0] = 0;
    }
    elm_transport_t io = {
        .send = tr_send,
        .restart = tr_restart,
        .discard_partial = tr_discard,
        .save_protocol = tr_save_protocol,
    };
    obd_state_init(&s_state);
    elm_init(&s_elm, &s_state, &io, proto);
    elm_assembler_reset(&s_asm);
}

static void set_mode(bool demo)
{
    s_demo = demo;
    if (demo) {
        if (s_ble_started) {
            obd_link_stop();
        }
        demo_start(&s_state, app_now_ms());
        ESP_LOGI(TAG, "demo mode");
    } else {
        start_session();
        if (!s_ble_started) {
            obd_link_init(s_events);
            s_ble_started = true;
        }
        obd_link_start();
        ESP_LOGI(TAG, "looking for the adapter");
    }
}

static void handle(const link_event_t *ev, int64_t now)
{
    if (s_demo) {
        return;   // late events from before the switch
    }
    switch (ev->type) {
    case LINK_EV_UP:
        snprintf(s_state.ble_name, sizeof(s_state.ble_name), "%s", obd_link_name());
        {
            char note[64];
            snprintf(note, sizeof(note), "link up: %s", s_state.ble_name);
            sd_log_note(note, now);
        }
        elm_assembler_reset(&s_asm);
        elm_on_link_up(&s_elm, now);
        break;
    case LINK_EV_DOWN:
        sd_log_note("link down", now);
        elm_on_link_down(&s_elm);
        s_state.phase = PHASE_SCAN;
        break;
    case LINK_EV_DATA:
        elm_assembler_feed(&s_asm, ev->data, ev->len, on_reply, NULL);
        break;
    case LINK_EV_PHASE:
        // Only while there is no session: once linked, the session owns the phase.
        if (s_elm.s == S_IDLE) {
            s_state.phase = (obd_phase_t)ev->data[0];
        }
        break;
    }
}

// A note in obd_raw.log at each phase change, so a log shows how long the
// scan, connect and init took.
static void note_phase(int64_t now)
{
    static const char *NAMES[] = {"no Bluetooth", "scanning", "connecting", "init", "ECU off", "live"};
    static int noted = -1;
    if ((int)s_state.phase != noted && s_state.phase <= PHASE_LIVE) {
        noted = s_state.phase;
        char note[32];
        snprintf(note, sizeof(note), "phase: %s", NAMES[noted]);
        sd_log_note(note, now);
    }
}

static void take_requests(int64_t now)
{
    uint32_t req = app_take_requests(REQ_CLEAR_CODES | REQ_FORGET_ADAPTER | REQ_DEMO_CHANGED);
    if (req & REQ_CLEAR_CODES) {
        if (s_demo) {
            demo_request_clear(&s_state, now);
        } else if (!elm_request_clear(&s_elm, now)) {
            s_state.clear = CLEAR_FAILED;
        }
    }
    if ((req & REQ_FORGET_ADAPTER) && !s_demo) {
        obd_link_forget();
        elm_forget_protocol(&s_elm);
        nvs_erase_("proto");
        obd_link_restart();
    }
    if (req & REQ_DEMO_CHANGED) {
        bool demo = app_settings()->demo;
        if (demo != s_demo) {
            set_mode(demo);
        }
    }
}

static void obd_task(void *arg)
{
    set_mode(app_settings()->demo);
    int64_t last_demo = 0;
    for (;;) {
        link_event_t ev;
        if (xQueueReceive(s_events, &ev, pdMS_TO_TICKS(TICK_MS)) == pdTRUE) {
            handle(&ev, app_now_ms());
            while (xQueueReceive(s_events, &ev, 0) == pdTRUE) {
                handle(&ev, app_now_ms());
            }
        }
        int64_t now = app_now_ms();
        take_requests(now);
        if (s_demo) {
            ble_scan_want_tpms(false);
            if (now - last_demo >= DEMO_PERIOD_MS) {
                last_demo = now;
                demo_feed(&s_state, now);
            }
        } else {
            // The tyre sensors share the adapter's scan (ble/ble_scan.c).
            ble_scan_want_tpms(tpms_service_wants_scan());
            obd_link_poll(now);
            elm_tick(&s_elm, now);
            note_phase(now);
        }
        app_lock();
        memcpy(&app_dash()->obd, &s_state, sizeof(s_state));
        app_dash()->demo = s_demo;
        app_unlock();
    }
}

void obd_task_start(void)
{
    s_events = xQueueCreate(32, sizeof(link_event_t));
    xTaskCreatePinnedToCore(obd_task, "obd", 6144, NULL, 5, NULL, 0);
}
