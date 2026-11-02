#include <math.h>
#include <string.h>
#include "held_peak.h"

#define MAX_GAP_MS 500

void held_reset(held_peak_t *h)
{
    memset(h, 0, sizeof(*h));
}

static int8_t sign_of(float v) { return v > 0 ? 1 : v < 0 ? -1 : 0; }

void held_add(held_peak_t *h, int dt_ms, float v)
{
    if (isnan(v) || dt_ms <= 0 || dt_ms > MAX_GAP_MS) {
        h->n = h->head = 0;   // a gap: the peaks stay, the second starts again
        h->open = false;
        if (isnan(v)) return;   // no value: only the gap
        dt_ms = 0;
    }
    int8_t s = sign_of(v);
    float a = fabsf(v);
    if (!h->open) {
        h->open = true;
        h->cur_min = a;
        h->cur_sign = s;
        h->cur_ms = dt_ms;
    } else {
        if (a < h->cur_min) h->cur_min = a;
        if (s != h->cur_sign) h->cur_sign = 0;
        h->cur_ms += dt_ms;
    }
    if (h->cur_ms < HELD_BUCKET_MS) return;
    h->open = false;
    h->min[h->head] = h->cur_min;
    h->sign[h->head] = h->cur_sign;
    h->head = (h->head + 1) % HELD_BUCKETS;
    if (h->n < HELD_BUCKETS) h->n++;
    if (h->n < HELD_BUCKETS) return;
    int8_t first = h->sign[0];
    float held = h->min[0];
    for (int i = 1; i < HELD_BUCKETS && first; i++) {
        if (h->sign[i] != first) first = 0;
        if (h->min[i] < held) held = h->min[i];
    }
    if (first > 0 && held > h->peak_pos) h->peak_pos = held;
    if (first < 0 && held > h->peak_neg) h->peak_neg = held;
}
