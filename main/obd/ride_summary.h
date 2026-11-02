/*
 * A ride's details on the card. sensor_task.c fills a ride_report_t and
 * ride_report_json() writes it as the ride folder's ride.json.
 * ride_summary_parse() reads ride.json back for the Rides page. Fields can be
 * missing (null values), so each one found sets a bit in `have`. Pure logic.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RIDE_JSON_FORMAT 2

enum {
    RS_DURATION = 1 << 0,
    RS_ENGINE = 1 << 1,
    RS_DISTANCE = 1 << 2,
    RS_AVG = 1 << 3,
    RS_MAX_KMH = 1 << 4,
    RS_MAX_RPM = 1 << 5,
    RS_MAX_COOLANT = 1 << 6,
    RS_MIN_VOLTS = 1 << 7,
    RS_LEAN = 1 << 8,
    RS_MAX_G = 1 << 9,
    RS_ALERTS = 1 << 10,
    RS_CODES = 1 << 11,
    RS_FUEL = 1 << 12,       // fuel_raw_l and k
    RS_L100 = 1 << 13,       // only from 1 km on
    RS_BATTERY = 1 << 14,    // batt_end; batt_start may still be -1
};

typedef struct {
    int ride;                // the folder number (set by the caller, not in the text)
    bool in_progress;        // written during the ride, not at its end
    uint32_t have;           // RS_ bits
    uint32_t duration_s, engine_s;
    float km, avg_kmh, max_kmh, max_rpm, max_coolant, min_volts;
    float lean_left, lean_right, max_g;
    uint32_t alerts;
    char codes[48];          // "P0118 P0562", empty for none
    float fuel_raw_l, l100_raw, k;
    int batt_start, batt_end;   // %, -1 = unknown
} ride_summary_t;

// False if the text isn't a whole ride.json.
bool ride_summary_parse(const char *text, ride_summary_t *out);

// Everything ride.json holds. Floats are NAN when unknown, ints -1, strings "".
typedef struct {
    float min_kpa, max_kpa, max_c;
    bool paired;
    int heard;                 // packets from its sensor this ride
    int min_rssi;              // the weakest of them, dBm; 0 = none
} ride_tyre_t;

typedef struct {
    int ride;
    char uid[9];               // 8 hex digits, random per ride
    bool in_progress;
    char start[21];            // when the ride opened, UTC: "2026-10-17T09:12:03Z", "" = clock not set
    char firmware[32];
    char bike[24];             // the bike profile's BIKE_NAME
    uint32_t duration_s, engine_s;
    float distance_km, avg_kmh, max_kmh, max_rpm, max_coolant_c, min_volts;
    float max_lean_left_deg, max_lean_right_deg, max_g;
    float gyro_bias_dps[3];    // the gyro's zero-rate offset learned at stops (x, y, z); NAN = not learned
    uint32_t alerts;
    char codes[96];            // "P0118 P0562", seen during the ride
    float fuel_raw_l, fuel_raw_l100, fuel_k;
    int fuel_model, fuel_coverage_pct;
    float since_fill_km, since_fill_raw_l;
    int fills;
    int batt_start_pct, batt_end_pct, batt_end_mv;
    float batt_avg_ma;               // the cell's mean current over the ride, - = discharging
    int batt_full_mah, batt_remaining_mah;   // the gauge's capacity when full, and left at the end; -1 = unknown
    bool batt_charging, batt_cell;   // a cell was fitted and read
    float max_chip_c, max_cell_c;    // the hottest the ESP32-S3 and the gauge beside the cell got
    ride_tyre_t tyre[2];       // front, rear
    char ble_name[32], adapter[32], protocol[4], ecu[12], vin[18], cal_id[2][17], cvn[2][9];
    uint32_t timeouts, reinits, link_drops, ecu_drops;
} ride_report_t;

// A report with every value unknown.
void ride_report_init(ride_report_t *r);

// The report as one line of JSON. Returns the length, or 0 if it didn't fit.
size_t ride_report_json(const ride_report_t *r, char *out, size_t n);
