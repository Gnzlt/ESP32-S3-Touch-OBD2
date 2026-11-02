#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/* BQ27220 battery fuel gauge (I2C address 0x55) on the shared BSP I2C bus. */

typedef struct {
    bool present;            // a cell is connected to the MX1.25 battery connector
    bool charging;           // gauge reports current flowing into the cell
    uint16_t voltage_mv;     // cell voltage
    int16_t current_ma;      // + charging, - discharging
    uint16_t soc_percent;    // state of charge 0..100
    uint16_t time_to_empty;  // minutes (0xFFFF = not discharging)
    // What the gauge thinks the cell holds when full, and holds now (mAh). Its
    // charge has been seen at 0 % at 3.80 V, with the cell half full by its
    // own voltage table: these show whether it learns the real capacity.
    uint16_t full_mah, remaining_mah;
    // The gauge's temperature: with no thermistor on the 2-pin cell connector
    // this is its own sensor, on the board beside the connector.
    float temp_c;
    bool overtemp;           // the gauge flagged over-temperature (charging or discharging)
} battery_info_t;

/* Create the gauge driver with Waveshare's 500 mAh battery profile. The driver
 * compares it with the profile stored in the gauge and rewrites it only if they
 * differ (the first run after the factory firmware does one ~6 s update). */
esp_err_t battery_init(void);

esp_err_t battery_read(battery_info_t *out);
