/*
 * A live OBD2 dashboard for motorcycles on the Waveshare
 * ESP32-S3-Touch-LCD-1.85B, reading the bike through a BLE ELM327 adapter.
 * See README.md for the pages and docs/ for how it works.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "app.h"
#include "board_power.h"
#include "log_export.h"
#include "mem_report.h"
#include "obd_task.h"
#include "sd_log.h"
#include "sensor_task.h"
#include "sensors/wallclock.h"
#include "ui/ui.h"

static const char *TAG = "dash";

void app_main(void)
{
    // After switching itself off (empty or too hot): sleeps on until that's over.
    board_power_wake_check();
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    mem_checkpoint("boot");
    app_shared_init();
    esp_log_level_set("esp32_s3_touch_lcd_1_85B", ESP_LOG_WARN);   // it logs every brightness step

    // Display first, backlight off: the splash fades it in.
    bsp_display_cfg_t cfg = {
        .lv_adapter_cfg = ESP_LV_ADAPTER_DEFAULT_CONFIG(),
        .rotation = ESP_LV_ADAPTER_ROTATE_0,
        .tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_NONE,
        .touch_flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    cfg.lv_adapter_cfg.task_stack_size = 10 * 1024;   // 6.4 KB used at most (Rides sheet); drawing runs in swdraw
    bsp_display_start_with_config(&cfg);
    bsp_display_brightness_set(0);
    mem_checkpoint("display");
    ui_start();
    mem_checkpoint("splash");
    wallclock_init();   // before the card: rides get a date

    sd_log_start();
    mem_checkpoint("sd log");
    log_export_start();
    mem_checkpoint("log export");
    sensor_task_start();
    mem_checkpoint("sensors");
    obd_task_start();
    mem_checkpoint("obd");
    ESP_LOGI(TAG, "started (%s)", app_settings()->demo ? "demo" : "looking for the adapter");

    // Every minute for the first 5 minutes, then every 10.
    for (int i = 0;; i++) {
        vTaskDelay(pdMS_TO_TICKS((i < 5 ? 60 : 600) * 1000));
        mem_report_tasks();
    }
}
