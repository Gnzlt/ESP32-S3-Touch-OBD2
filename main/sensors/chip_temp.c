#include "chip_temp.h"

#include "driver/temperature_sensor.h"
#include "esp_log.h"

static const char *TAG = "chiptemp";
static temperature_sensor_handle_t s_tsens;

bool chip_temp_init(void)
{
    if (s_tsens) return true;   // board_power_wake_check() got here first
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
    if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK || temperature_sensor_enable(s_tsens) != ESP_OK) {
        ESP_LOGW(TAG, "temperature sensor didn't start");
        s_tsens = NULL;
        return false;
    }
    return true;
}

bool chip_temp_read(float *celsius)
{
    return s_tsens && temperature_sensor_get_celsius(s_tsens, celsius) == ESP_OK;
}
