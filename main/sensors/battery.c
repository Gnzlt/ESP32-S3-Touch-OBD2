#include "battery.h"

#include "bq27220.h"
#include "bsp/esp-bsp.h"

/* Battery profile for the optional 3.7 V 500 mAh cell, copied from Waveshare's
 * 08_bq27220_test / 01_comprehensive_example. bq27220_create() compares it with
 * the profile stored in the gauge and only writes when they differ. Driver
 * espressif/bq27220 >= 0.1.3 verifies more fields than the version in the
 * factory firmware, so the first boot after the factory app updates it once. */
static const parameter_cedv_t s_cedv = {
    .full_charge_cap = 500, .design_cap = 500, .reserve_cap = 0, .near_full = 200,
    .self_discharge_rate = 20,
    .EDV0 = 3490, .EDV1 = 3511, .EDV2 = 3535, .EMF = 3670,
    .C0 = 115, .R0 = 968, .T0 = 4547, .R1 = 4764, .TC = 11, .C1 = 0,
    .DOD0 = 4147, .DOD10 = 4002, .DOD20 = 3969, .DOD30 = 3938, .DOD40 = 3880,
    .DOD50 = 3824, .DOD60 = 3794, .DOD70 = 3753, .DOD80 = 3677, .DOD90 = 3574,
    .DOD100 = 3490,
};

static const gauging_config_t s_gauging = {
    .CCT = 1, .CSYNC = 0, .EDV_CMP = 0, .SC = 1, .FIXED_EDV0 = 0,
    .FCC_LIM = 1, .FC_FOR_VDQ = 1, .IGNORE_SD = 1, .SME0 = 0,
};

static bq27220_handle_t s_gauge;

esp_err_t battery_init(void)
{
    if (s_gauge) return ESP_OK;   // board_power_wake_check() got here first
    bq27220_config_t cfg = {
        .i2c_bus = bsp_i2c_bus_get_handle(),
        .cfg = &s_gauging,
        .cedv = &s_cedv,
    };
    s_gauge = bq27220_create(&cfg);
    return s_gauge ? ESP_OK : ESP_FAIL;
}

esp_err_t battery_read(battery_info_t *out)
{
    battery_status_t status;
    esp_err_t err = bq27220_get_battery_status(s_gauge, &status);
    if (err != ESP_OK) {
        return err;
    }
    out->present = status.BATTPRES;
    out->voltage_mv = bq27220_get_voltage(s_gauge);
    out->current_ma = bq27220_get_current(s_gauge);
    out->soc_percent = bq27220_get_state_of_charge(s_gauge);
    out->time_to_empty = bq27220_get_time_to_empty(s_gauge);
    out->full_mah = bq27220_get_full_charge_capacity(s_gauge);
    out->remaining_mah = bq27220_get_remaining_capacity(s_gauge);
    out->charging = !status.DSG && out->current_ma > 0;
    out->temp_c = bq27220_get_temperature(s_gauge) / 10.0f - 273.15f;   // 0.1 K
    out->overtemp = status.OTC || status.OTD;
    return ESP_OK;
}
