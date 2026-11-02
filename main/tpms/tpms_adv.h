/*
 * One BLE advertisement, split into the AD structures the TPMS formats use
 * (docs/tyre-sensors.md, "Formats"). A packet cut short still yields
 * what it holds: the structure it ends in keeps its first bytes, and
 * `truncated` says so. Pure C, no ESP-IDF.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define TPMS_ADV_UUIDS 8

typedef struct {
    char name[32];               // "" without one; non-ASCII bytes become '?'
    uint16_t uuids16[TPMS_ADV_UUIDS];
    int n_uuids;
    // The first manufacturer-specific value, its 2-byte company id first.
    // Points into the raw packet, which must outlive the decode.
    const uint8_t *mfr;
    int mfr_len;                 // -1 without one
    // The first 16-bit service data: its UUID, and the data after it.
    int service_uuid;            // -1 without one
    const uint8_t *service_data;
    int service_len;
    bool truncated;              // the packet ended inside an AD structure
} tpms_adv_t;

void tpms_adv_parse(tpms_adv_t *a, const uint8_t *raw, int len);
// The manufacturer data's company id, or -1 without one.
int tpms_adv_company(const tpms_adv_t *a);
bool tpms_adv_has_uuid(const tpms_adv_t *a, uint16_t uuid);
