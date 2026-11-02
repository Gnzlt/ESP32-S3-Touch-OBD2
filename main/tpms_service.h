/*
 * The tyre sensors on the dashboard: listens through the shared BLE scan
 * (ble/ble_scan.c), decodes with tpms/, keeps each paired tyre's state, runs
 * pairing, and keeps the pairings and alert limits in NVS and in
 * /obd2/tpms.cfg on the card (a card that has the file wins, so it sets up
 * a fresh board). Runs in the sensor task: the scan callback only copies
 * packets into a queue, and the UI sends commands through another, so all the
 * state belongs to one task. The UI reads it from dash_t.tpms.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tpms/tpms_alerts.h"
#include "tpms/tpms_pair.h"
#include "tpms/tpms_tyres.h"

#define TPMS_STALE_MS   120000   // a reading older than this shows as "no signal"
#define TPMS_PAIR_MS    60000    // how long pairing listens

typedef struct {
    tpms_pairing_t sensor;       // pass this back to pair it
    float kpa;
    int rssi;
    int on_tyre;                 // the tyre it is paired to already, -1 for none
} tpms_cand_view_t;

// What the UI sees, published into dash_t every 100 ms.
typedef struct {
    bool on;                     // the "Tyre sensors" setting
    bool demo;                   // synthetic tyres (demo mode)
    tpms_pairing_t pair[TPMS_TYRES];
    tpms_tyre_t tyre[TPMS_TYRES];
    int rssi[TPMS_TYRES];        // of the tyre's last packet, 0 = none
    // This ride's lowest and highest pressure and highest temperature, NAN before any.
    float ride_min_kpa[TPMS_TYRES], ride_max_kpa[TPMS_TYRES], ride_max_c[TPMS_TYRES];
    int ride_heard[TPMS_TYRES];        // packets this ride, sleep frames too
    int ride_min_rssi[TPMS_TYRES];     // the weakest of them, 0 = none
    // The latest reading worth a line in events.csv: a changed value, or a
    // minute since the last. reading_seq counts them.
    uint32_t reading_seq[TPMS_TYRES];
    float reading_kpa[TPMS_TYRES], reading_c[TPMS_TYRES];
    int reading_rssi[TPMS_TYRES];
    tpms_cfg_t cfg;
    int pairing;                 // the tyre being paired, -1 for none
    int pair_left_s;             // seconds of listening left
    int n_cands;
    tpms_cand_view_t cands[TPMS_CAND_MAX];   // strongest first
} tpms_view_t;

void tpms_service_init(void);
// Every sensor-task tick: packets, commands, the card's config file.
void tpms_service_poll(int64_t now, bool demo);
// With app_lock held.
void tpms_service_publish(tpms_view_t *out, int64_t now);
// Once a second: the most urgent tyre alert that just fired, with its tyre.
// riding: moving (for the silent-sensor alert).
tpms_alert_t tpms_service_alert(int64_t now, bool riding, int *tyre);
// A ride opened: the once-a-ride alerts (sensor battery) may fire again, and
// the ride's extremes start again.
void tpms_service_new_ride(void);
// Whether the scan should listen for tyre sensors (the OBD task asks).
bool tpms_service_wants_scan(void);

// From the UI.
void tpms_service_pair_start(int tyre);
void tpms_service_pair_stop(void);
void tpms_service_pair_pick(int tyre, const tpms_pairing_t *sensor);
void tpms_service_forget(int tyre);
void tpms_service_set_cfg(const tpms_cfg_t *cfg);

// "front" / "rear"; "FRONT" / "REAR".
extern const char *const TPMS_TYRE_NAMES[TPMS_TYRES];
extern const char *const TPMS_TYRE_CAPS[TPMS_TYRES];
