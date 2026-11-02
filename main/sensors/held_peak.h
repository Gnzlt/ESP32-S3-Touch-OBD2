/*
 * The largest value held for a full second, each side of zero. A jolt or the
 * board shifting in its bag lasts a fraction of a second and doesn't count; a
 * lean held through a corner does. Values go into 100 ms buckets (each keeps
 * its smallest |value| and whether its sign stayed the same); when the last
 * ten all share one sign, their smallest is a value held that long. Pure C,
 * host-tested.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define HELD_BUCKET_MS 100
#define HELD_BUCKETS   10      // a second

typedef struct {
    float min[HELD_BUCKETS];
    int8_t sign[HELD_BUCKETS];
    int n, head;               // full buckets in the ring, and where the next goes
    bool open;                 // a bucket is being filled
    float cur_min;
    int8_t cur_sign;
    int cur_ms;
    float peak_pos, peak_neg;  // the largest held each side, as positive numbers; 0 = none
} held_peak_t;

void held_reset(held_peak_t *h);
// dt_ms since the previous value. A gap (over 500 ms, or a NAN for "no value")
// starts the second again.
void held_add(held_peak_t *h, int dt_ms, float v);
