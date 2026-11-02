/*
 * What the tasks share. The OBD task owns the ELM327 session and the sensor
 * task owns the IMU, battery, alerts and trip; each publishes into one
 * snapshot under a mutex, and the UI copies the snapshot ~30 times a second.
 * Requests from the UI (clear codes, reset trip, ...) go the other way as
 * flags the owning task picks up.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "obd/alerts.h"
#include "obd/obd_state.h"
#include "obd/fuel.h"
#include "obd/trip.h"
#include "sensors/battery.h"
#include "tpms_service.h"

#define COOLANT_HIST 60          // one sample every 10 s: the last 10 minutes

typedef struct {
    bool ok;                     // the IMU answers
    bool calibrated;             // "up" came from a zero or from riding, not the boot guess
    bool valid;                  // riding fast enough for lean and G (else they read 0)
    float lean;                  // degrees, + right, - left
    float lat_g, lon_g;          // + right / + accelerating
    float pitch;                 // degrees, nose up +
} motion_t;

typedef struct {
    int64_t now;                 // ms since boot, when this was taken
    obd_state_t obd;
    int gear;                    // 0 = unknown / neutral / clutch in
    motion_t motion;
    trip_t trip;
    fill_t fill;                 // fuel since the last fill-up (raw; times fill.k on screen)
    float fuel_lph;              // raw estimated flow now, < 0 = unknown
    battery_info_t battery;
    bool battery_ok;
    float chip_c;                // the ESP32-S3's die temperature, NAN unknown
    // For an ALERT_BOARD_HOT: which part, whether critical, and how hot (NAN
    // when the gauge's over-temperature flag alone raised it).
    hot_source_t hot_source;
    bool hot_critical;
    float hot_c;
    alert_t alert;               // the latest alert
    int alert_tyre;              // for a tyre alert: TPMS_FRONT or TPMS_REAR
    uint32_t alert_seq;          // bumps every time an alert fires
    float coolant_hist[COOLANT_HIST];
    int coolant_hist_n;
    uint32_t coolant_hist_seq;   // bumps with every new sample
    tpms_view_t tpms;            // the tyre sensors
    bool demo;
    uint8_t switching_off;       // a guard_action_t (GUARD_OFF_*): the board is about to sleep; 0 = no
} dash_t;

typedef struct {
    int brightness;              // 10..100 %
    bool beeps;
    bool demo;
    bool alerts;
    bool sd_log;                 // ride logs on the microSD card
    // Appended 2026-10-16; older saved settings are read up to here (settings_load).
    bool tpms;                   // listen for the tyre sensors
    uint8_t pressure_unit;       // tpms_unit_t
    // Appended 2026-10-16 into the padding, so older blobs load at full size (0 there).
    uint8_t saver;               // battery saver: the dim status screen (ui.c)
} settings_t;

// Requests from the UI.
typedef enum {
    REQ_CLEAR_CODES = 1 << 0,
    REQ_RESET_TRIP = 1 << 1,
    REQ_ZERO_LEAN = 1 << 2,
    REQ_FORGET_ADAPTER = 1 << 3,
    REQ_SETTINGS_CHANGED = 1 << 4,   // beeps, alerts, saver (sensor task)
    REQ_DEMO_CHANGED = 1 << 6,       // demo on/off (OBD task)
    REQ_RESET_LEAN_MAX = 1 << 5,
    REQ_FILL_UP = 1 << 7,            // a fill-up, litres in app_take_fill_litres() (sensor task)
} app_request_t;

int64_t app_now_ms(void);

void app_shared_init(void);
void app_lock(void);
void app_unlock(void);
// The shared snapshot; touch it only between app_lock()/app_unlock().
dash_t *app_dash(void);
// A copy for the UI.
void app_snapshot(dash_t *out);

void app_request(app_request_t r);
// Takes (and clears) the given requests; returns the ones that were set.
uint32_t app_take_requests(uint32_t mask);
// A fill-up of this many litres (raises REQ_FILL_UP); the sensor task takes the litres.
void app_request_fill(float litres);
float app_take_fill_litres(void);

// Settings, saved in NVS.
void settings_load(settings_t *s);
void settings_save(const settings_t *s);
settings_t *app_settings(void);   // the live copy; change it, then app_settings_changed()
// Save the settings and tell the tasks.
void app_settings_changed(void);

// Small NVS helpers for the tasks' own state.
bool nvs_get_blob_(const char *key, void *out, size_t size);
void nvs_set_blob_(const char *key, const void *data, size_t size);
void nvs_erase_(const char *key);
