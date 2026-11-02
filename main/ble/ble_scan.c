#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "host/ble_hs.h"
#include "ble_scan.h"

static const char *TAG = "scan";

typedef enum { M_OFF, M_ADAPTER, M_TPMS, M_TPMS_CONNECTED } mode_t_;

#define RETRY_MS 1000

static SemaphoreHandle_t s_mutex;
static bool s_synced;
static uint8_t s_own_addr_type;
static bool s_want_adapter, s_want_tpms, s_connected, s_paused;
static volatile bool s_running;         // cleared by DISC_COMPLETE in the host task
static mode_t_ s_mode = M_OFF;          // the mode the running scan was started in
static int64_t s_retry_at;
static ble_scan_sink_t s_adapter_sink, s_tpms_sink;

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
        if (s_adapter_sink) s_adapter_sink(&ev->disc);
        if (s_tpms_sink) s_tpms_sink(&ev->disc);
        return 0;
    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_running = false;   // cancelled, or stopped by the stack: ble_scan_update() restarts it if wanted
        return 0;
    }
    return 0;
}

static mode_t_ wanted(void)
{
    if (!s_synced || s_paused) return M_OFF;
    if (s_want_adapter) return M_ADAPTER;
    if (s_want_tpms) return s_connected ? M_TPMS_CONNECTED : M_TPMS;
    return M_OFF;
}

static void start(mode_t_ m)
{
    // Units of 0.625 ms.
    struct ble_gap_disc_params p = {.filter_duplicates = 0};
    switch (m) {
    case M_ADAPTER: p.itvl = 0x50, p.window = 0x40, p.passive = 0; break;
    case M_TPMS: p.itvl = 0xA0, p.window = 0x50, p.passive = 1; break;
    default: p.itvl = 0xA0, p.window = 0x30, p.passive = 1; break;
    }
    int rc = ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &p, gap_event, NULL);
    if (rc == 0) {
        s_running = true;
        s_mode = m;
    } else {
        ESP_LOGW(TAG, "scan failed to start (rc %d)", rc);
        s_retry_at = esp_timer_get_time() / 1000 + RETRY_MS;
    }
}

void ble_scan_update(void)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    mode_t_ m = wanted();
    if (s_running && (m != s_mode)) {
        ble_gap_disc_cancel();   // no DISC_COMPLETE follows a cancel
        s_running = false;
    }
    if (!s_running && ble_gap_disc_active()) {
        s_running = true;        // the stack still runs it: leave it
    }
    if (m != M_OFF && !s_running && esp_timer_get_time() / 1000 >= s_retry_at) {
        start(m);
    }
    xSemaphoreGive(s_mutex);
}

void ble_scan_ready(uint8_t own_addr_type)
{
    if (!s_mutex) s_mutex = xSemaphoreCreateMutex();
    s_own_addr_type = own_addr_type;
    s_synced = true;
    s_running = false;
    s_retry_at = 0;
    ble_scan_update();
}

void ble_scan_lost(void)
{
    s_synced = false;
    s_running = false;
}

void ble_scan_want_adapter(bool on) { s_want_adapter = on; ble_scan_update(); }
void ble_scan_want_tpms(bool on)
{
    if (on == s_want_tpms) return;
    s_want_tpms = on;
    ble_scan_update();
}
void ble_scan_set_connected(bool connected) { s_connected = connected; ble_scan_update(); }
void ble_scan_pause(void) { s_paused = true; ble_scan_update(); }
void ble_scan_resume(void) { s_paused = false; ble_scan_update(); }

void ble_scan_set_adapter_sink(ble_scan_sink_t sink) { s_adapter_sink = sink; }
void ble_scan_set_tpms_sink(ble_scan_sink_t sink) { s_tpms_sink = sink; }
