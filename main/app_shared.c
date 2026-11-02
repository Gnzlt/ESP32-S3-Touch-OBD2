#include <math.h>
#include <stddef.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "app.h"

#define NVS_NS "obd2dash"

static SemaphoreHandle_t s_mutex;
static EXT_RAM_BSS_ATTR dash_t s_dash;
static settings_t s_settings;
static volatile uint32_t s_requests;
static portMUX_TYPE s_req_mux = portMUX_INITIALIZER_UNLOCKED;
static float s_fill_litres;   // guarded by s_req_mux

int64_t app_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

void app_shared_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    memset(&s_dash, 0, sizeof(s_dash));
    s_dash.chip_c = s_dash.hot_c = NAN;
    obd_state_init(&s_dash.obd);
    settings_load(&s_settings);
    s_dash.demo = s_settings.demo;
}

void app_lock(void) { xSemaphoreTake(s_mutex, portMAX_DELAY); }
void app_unlock(void) { xSemaphoreGive(s_mutex); }
dash_t *app_dash(void) { return &s_dash; }

void app_snapshot(dash_t *out)
{
    app_lock();
    memcpy(out, &s_dash, sizeof(*out));
    app_unlock();
    out->now = app_now_ms();
}

void app_request(app_request_t r)
{
    portENTER_CRITICAL(&s_req_mux);
    s_requests |= r;
    portEXIT_CRITICAL(&s_req_mux);
}

uint32_t app_take_requests(uint32_t mask)
{
    portENTER_CRITICAL(&s_req_mux);
    uint32_t got = s_requests & mask;
    s_requests &= ~mask;
    portEXIT_CRITICAL(&s_req_mux);
    return got;
}

void app_request_fill(float litres)
{
    portENTER_CRITICAL(&s_req_mux);
    s_fill_litres = litres;
    s_requests |= REQ_FILL_UP;
    portEXIT_CRITICAL(&s_req_mux);
}

float app_take_fill_litres(void)
{
    portENTER_CRITICAL(&s_req_mux);
    float l = s_fill_litres;
    portEXIT_CRITICAL(&s_req_mux);
    return l;
}

settings_t *app_settings(void) { return &s_settings; }

void app_settings_changed(void)
{
    settings_save(&s_settings);
    app_request(REQ_SETTINGS_CHANGED | REQ_DEMO_CHANGED);
}

bool nvs_get_blob_(const char *key, void *out, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    size_t len = size;
    esp_err_t err = nvs_get_blob(h, key, out, &len);
    nvs_close(h);
    return err == ESP_OK && len == size;
}

void nvs_set_blob_(const char *key, const void *data, size_t size)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_blob(h, key, data, size);
    nvs_commit(h);
    nvs_close(h);
}

void nvs_erase_(const char *key)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_erase_key(h, key);
    nvs_commit(h);
    nvs_close(h);
}

void settings_load(settings_t *s)
{
    s->brightness = 85;
    s->beeps = true;
    s->alerts = true;
    s->sd_log = true;
#if CONFIG_DASH_DEMO_DEFAULT
    s->demo = true;
#else
    s->demo = false;
#endif
    s->tpms = true;
    s->pressure_unit = TPMS_UNIT_BAR;
    s->saver = 0;
    settings_t saved;
    if (nvs_get_blob_("settings", &saved, sizeof(saved))) {
        *s = saved;
    } else if (nvs_get_blob_("settings", &saved, offsetof(settings_t, tpms))) {
        // Saved before the tyre settings: keep what it has, defaults for the rest.
        memcpy(s, &saved, offsetof(settings_t, tpms));
    }
    if (s->saver > 1) {
        s->saver = 0;
    }
    if (s->pressure_unit >= TPMS_UNIT_COUNT) {
        s->pressure_unit = TPMS_UNIT_BAR;
    }
    if (s->brightness < 10 || s->brightness > 100) {
        s->brightness = 85;
    }
#if CONFIG_DASH_SCREENSHOTS
    // README screenshots are of the demo ride, whatever the board has saved (not written back).
    s->demo = true;
    s->saver = 0;
#endif
}

void settings_save(const settings_t *s)
{
    nvs_set_blob_("settings", s, sizeof(*s));
}
