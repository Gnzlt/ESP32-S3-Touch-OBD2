/*
 * Which sensor belongs to which tyre, and the sensors heard while pairing.
 * A sensor is known by the id in its payload when its format has one, and by
 * its BLE address otherwise (the ESP32 sees the address).
 * Pairing lists the strongest first: the closest sensor is usually the tyre
 * being paired. Pure C.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "tpms_formats.h"

// One tyre's sensor, saved in NVS as a blob. format 0 = no sensor.
typedef struct {
    uint8_t format;
    uint8_t id_len;
    uint8_t id[TPMS_ID_MAX];
    uint8_t addr[6];             // NimBLE order: addr[0] is the least significant byte
} tpms_pairing_t;

void tpms_pairing_set(tpms_pairing_t *p, const tpms_reading_t *r, const uint8_t addr[6]);
bool tpms_pairing_matches(const tpms_pairing_t *p, const tpms_reading_t *r, const uint8_t addr[6]);
// Whether two records are the same sensor (no sensor is never the same as anything).
bool tpms_pairing_same(const tpms_pairing_t *a, const tpms_pairing_t *b);
// The last 3 bytes of the sensor's id as hex ("445566"): enough to tell two
// apart, and what a kit's display or app shows as "ID:3CAB72445566" ends with. Without a
// payload id, the end of the address as it is usually printed.
void tpms_pairing_tail(const tpms_pairing_t *p, char out[7]);

#define TPMS_CAND_MAX      8
#define TPMS_CAND_FRESH_MS 20000   // a sensor unheard this long has moved away, or stopped sending

typedef struct {
    tpms_pairing_t sensor;       // who it is, ready to pair
    tpms_reading_t reading;      // its latest reading
    int rssi;
    int64_t at;
} tpms_cand_t;

typedef struct {
    tpms_cand_t c[TPMS_CAND_MAX];
    int n;
} tpms_cands_t;

void tpms_cands_clear(tpms_cands_t *l);
void tpms_cands_offer(tpms_cands_t *l, const tpms_reading_t *r, const uint8_t addr[6], int rssi, int64_t now);
// The sensors heard in the last TPMS_CAND_FRESH_MS, strongest first, as indexes into l->c.
int tpms_cands_fresh(const tpms_cands_t *l, int64_t now, int out[TPMS_CAND_MAX]);
