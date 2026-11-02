/*
 * The broadcast BLE TPMS formats: which one an advertisement is in, and what
 * it says about its tyre. Every decoder returns gauge kPa and °C; a field the
 * packet did not carry (another frame type, or cut short) is NAN. The published
 * captures of each format are the host-test fixtures (test/host/test_tpms.c);
 * README.md credits the projects the formats were learned from.
 *
 * The services-UUID formats are tried first: a SYTPMS sensor's first two
 * bytes are its status and voltage, which can look like any company id.
 * DJTPMS comes next, since its exact size and CRC are a tighter match than a
 * company id.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "tpms_adv.h"

typedef enum {
    TPMS_F_NONE,
    TPMS_F_SYSGRATION,
    TPMS_F_SYTPMS,
    TPMS_F_MICHELIN,
    TPMS_F_WODHMIEY,
    TPMS_F_FOBO,
    TPMS_F_BEKUBEE,
    TPMS_F_TESLA,
    TPMS_F_DJTPMS,
    TPMS_F_COUNT
} tpms_format_t;

extern const char *const TPMS_FORMAT_NAMES[TPMS_F_COUNT];

#define TPMS_ID_MAX 6

typedef struct {
    tpms_format_t format;
    // The sensor's own position code when its format has one (Sysgration's
    // 0x80 + n gives n), else -1. A hint, never trusted alone.
    int position;
    // The id in the payload (a MAC or the printed id), when the format has one.
    uint8_t id[TPMS_ID_MAX];
    uint8_t id_len;
    float kpa;                   // gauge
    float temp_c;
    float volts;
    int battery_pct;             // -1 = not sent
    // The sensor's own alarm (leak, no pressure). The rider's limits are the
    // alert engine's business, not the decoder's.
    bool alarm;
    // False for a sleep frame: the sensor is alive but sent no reading.
    bool awake;
} tpms_reading_t;

// The reading in a, or false if a is no TPMS advertisement we know.
bool tpms_decode(const tpms_adv_t *a, tpms_reading_t *r);

// DJTPMS's CRC-8: polynomial 0x2F, initial value 0xDF, MSB first.
uint8_t tpms_djtpms_crc8(const uint8_t *b, int n);
