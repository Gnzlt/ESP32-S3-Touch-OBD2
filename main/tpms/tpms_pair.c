#include <stdio.h>
#include <string.h>
#include "tpms_pair.h"

void tpms_pairing_set(tpms_pairing_t *p, const tpms_reading_t *r, const uint8_t addr[6])
{
    memset(p, 0, sizeof(*p));
    p->format = (uint8_t)r->format;
    p->id_len = r->id_len;
    memcpy(p->id, r->id, r->id_len);
    memcpy(p->addr, addr, 6);
}

bool tpms_pairing_matches(const tpms_pairing_t *p, const tpms_reading_t *r, const uint8_t addr[6])
{
    if (p->format == 0 || p->format != r->format) {
        return false;
    }
    if (p->id_len > 0 && r->id_len > 0) {
        return p->id_len == r->id_len && memcmp(p->id, r->id, r->id_len) == 0;
    }
    return memcmp(p->addr, addr, 6) == 0;
}

bool tpms_pairing_same(const tpms_pairing_t *a, const tpms_pairing_t *b)
{
    if (a->format == 0 || a->format != b->format) {
        return false;
    }
    if (a->id_len > 0 && b->id_len > 0) {
        return a->id_len == b->id_len && memcmp(a->id, b->id, a->id_len) == 0;
    }
    return memcmp(a->addr, b->addr, 6) == 0;
}

void tpms_pairing_tail(const tpms_pairing_t *p, char out[7])
{
    if (p->id_len >= 3) {
        const uint8_t *t = p->id + p->id_len - 3;
        snprintf(out, 7, "%02X%02X%02X", t[0], t[1], t[2]);
    } else {
        snprintf(out, 7, "%02X%02X%02X", p->addr[2], p->addr[1], p->addr[0]);
    }
}

void tpms_cands_clear(tpms_cands_t *l)
{
    l->n = 0;
}

void tpms_cands_offer(tpms_cands_t *l, const tpms_reading_t *r, const uint8_t addr[6], int rssi, int64_t now)
{
    tpms_cand_t c = {.reading = *r, .rssi = rssi, .at = now};
    tpms_pairing_set(&c.sensor, r, addr);
    for (int i = 0; i < l->n; i++) {
        if (tpms_pairing_matches(&l->c[i].sensor, r, addr)) {
            l->c[i] = c;
            return;
        }
    }
    if (l->n < TPMS_CAND_MAX) {
        l->c[l->n++] = c;
        return;
    }
    // Full: the new one replaces the weakest, if it is stronger.
    int weakest = 0;
    for (int i = 1; i < l->n; i++) {
        if (l->c[i].rssi < l->c[weakest].rssi) weakest = i;
    }
    if (rssi > l->c[weakest].rssi) l->c[weakest] = c;
}

int tpms_cands_fresh(const tpms_cands_t *l, int64_t now, int out[TPMS_CAND_MAX])
{
    int n = 0;
    for (int i = 0; i < l->n; i++) {
        if (now - l->c[i].at > TPMS_CAND_FRESH_MS) continue;
        int j = n++;
        while (j > 0 && l->c[out[j - 1]].rssi < l->c[i].rssi) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = i;
    }
    return n;
}
