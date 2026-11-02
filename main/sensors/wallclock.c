#include "wallclock.h"

#include <sys/time.h>
#include <time.h>
#include "esp_log.h"
#include "i2c_bus.h"
#include "bsp/esp-bsp.h"
#include "rtc_regs.h"

static const char *TAG = "clock";

#define RTC_ADDR   0x51
#define VALID_FROM 1767225600   // 2026-01-01: anything earlier is a clock that was never set

static i2c_bus_device_handle_t s_dev;

static void set_system_time(int64_t epoch)
{
    struct timeval tv = {.tv_sec = (time_t)epoch};
    settimeofday(&tv, NULL);
}

void wallclock_init(void)
{
    s_dev = i2c_bus_device_create(bsp_i2c_bus_get_handle(), RTC_ADDR, 0);
    uint8_t regs[RTC_REGS_N];
    int64_t epoch;
    if (!s_dev || i2c_bus_read_bytes(s_dev, RTC_REGS_FIRST, RTC_REGS_N, regs) != ESP_OK) {
        ESP_LOGW(TAG, "clock not answering");
        return;
    }
    if (!rtc_regs_decode(regs, &epoch) || epoch < VALID_FROM) {
        ESP_LOGI(TAG, "clock not set");
        return;
    }
    set_system_time(epoch);
    char text[24];
    time_t t = (time_t)epoch;
    strftime(text, sizeof(text), "%Y-%m-%d %H:%M:%S", gmtime(&t));
    ESP_LOGI(TAG, "clock: %s UTC", text);
}

bool wallclock_set(int64_t epoch)
{
    if (epoch < VALID_FROM) return false;
    set_system_time(epoch);
    uint8_t regs[RTC_REGS_N];
    rtc_regs_encode(epoch, regs);
    if (!s_dev || i2c_bus_write_bytes(s_dev, RTC_REGS_FIRST, RTC_REGS_N, regs) != ESP_OK) {
        ESP_LOGW(TAG, "can't write the clock");
        return false;
    }
    return true;
}

bool wallclock_valid(void)
{
    return time(NULL) >= VALID_FROM;
}
