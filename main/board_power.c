#include "board_power.h"

#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "sensors/battery.h"
#include "sensors/chip_temp.h"

static const char *TAG = "power";

static RTC_DATA_ATTR int s_slept_for;   // the guard's reason, kept through deep sleep

const char *board_power_reason(guard_action_t why)
{
    return why == GUARD_OFF_EMPTY ? "battery empty" : why == GUARD_OFF_HOT ? "too hot" : "";
}

static void sleep_now(guard_action_t why)
{
    s_slept_for = why;
    // The pads let go in deep sleep: hold the backlight's low.
    gpio_hold_en(BSP_LCD_BACKLIGHT);
    gpio_deep_sleep_hold_en();
    esp_sleep_enable_timer_wakeup((uint64_t)GUARD_SLEEP_S * 1000000ULL);
    esp_deep_sleep_start();
}

void board_power_sleep(guard_action_t why)
{
    ESP_LOGW(TAG, "%s: deep sleep, checking every %d s", board_power_reason(why), GUARD_SLEEP_S);
    bsp_display_brightness_set(0);
    sleep_now(why);
}

guard_action_t board_power_wake_check(void)
{
    gpio_hold_dis(BSP_LCD_BACKLIGHT);
    gpio_deep_sleep_hold_dis();
    guard_action_t why = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER ? (guard_action_t)s_slept_for
                                                                                  : GUARD_NONE;
    s_slept_for = GUARD_NONE;
    if (why != GUARD_OFF_EMPTY && why != GUARD_OFF_HOT) {
        return GUARD_NONE;
    }
    guard_in_t in = {0};
    battery_info_t b;
    if (bsp_i2c_init() == ESP_OK && battery_init() == ESP_OK && battery_read(&b) == ESP_OK && b.present) {
        in.battery = in.has_cell = true;
        in.mv = b.voltage_mv;
        in.charging = b.charging;
        in.cell_c = b.temp_c;
    }
    float c;
    if (chip_temp_init() && chip_temp_read(&c)) {
        in.has_chip = true;
        in.chip_c = c;
    }
    if (!guard_may_wake(why, &in)) {
        ESP_LOGI(TAG, "still %s (%d mV, cell %.0f C, chip %.0f C): back to sleep", board_power_reason(why), in.mv,
                 in.has_cell ? in.cell_c : 0.0f, in.has_chip ? in.chip_c : 0.0f);
        sleep_now(why);
    }
    ESP_LOGI(TAG, "no longer %s: starting", board_power_reason(why));
    return why;
}
