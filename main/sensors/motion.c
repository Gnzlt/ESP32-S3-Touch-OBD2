#include <math.h>
#include <string.h>
#include "motion.h"

#define G          9.80665f
#define DEG        (180.0f / (float)M_PI)
#define RAD        ((float)M_PI / 180.0f)
#define BOOT_N     50        // ~1 s of samples to guess "up" when there is no zero
#define LEAN_ON_KMH  15.0f   // lean and G count from here...
#define LEAN_OFF_KMH 10.0f   // ...until below this
#define ROLL_KEEP  0.97f     // complementary filter: gyro roll rate vs the turn's lean
#define G_SMOOTH   0.15f
#define UP_TAU_NEW 1.0f      // s: learning "up" fast while it is only a guess
#define UP_TAU     3.0f      // s: then slowly, following a loose case that shifts
#define UP_SETTLE  5.0f      // s of riding before the learned up counts as calibrated
#define UP_GATE    0.3f      // |accel| this far from 1 g (bumps, hard braking): not learned from
// The board moving in its case (seen in a tank bag): lean flipped +41 to
// -24 to -70 degrees within 1.4 s and lateral g hit -3.6. A bike doesn't turn
// or roll this fast; a jolt does. Such a sample is skipped, and for a while
// after it "up" is learned afresh (the board may now sit differently) and the
// lean follows the turn rather than the gyro's roll.
#define JOLT_DPS   200.0f
#define SHAKEN_S   3.0f
#define SHAKEN_KEEP 0.9f     // per sample after a jolt: the turn estimate takes over in ~0.2 s
#define LAT_MAX    1.2f      // g; ~50 degrees, past what a road bike reaches
// The gyro's zero-rate offset. Uncorrected, a ride leaned ~10 degrees left on every
// straight, more the faster: an offset read as a steady turn. Standing still
// (speed 0) nothing turns, so the gyro's average then is its offset. The
// engine's shake averages out; faster than BIAS_GATE is the board being
// handled, and away from 1 g it is being moved.
#define BIAS_TAU   2.0f      // s
#define BIAS_GATE  30.0f     // deg/s, any axis
#define BIAS_ACCEL_GATE 0.1f // |accel| this far from 1 g: not learned from
#define STILL_KMH  0.5f

static float dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void cross(const float a[3], const float b[3], float o[3])
{
    o[0] = a[1] * b[2] - a[2] * b[1];
    o[1] = a[2] * b[0] - a[0] * b[2];
    o[2] = a[0] * b[1] - a[1] * b[0];
}

static bool normalize(float v[3])
{
    float n = sqrtf(dot(v, v));
    if (n < 1e-6f) return false;
    v[0] /= n; v[1] /= n; v[2] /= n;
    return true;
}

static void build_frame(motion_est_t *m)
{
    // Forward: the screen normal (z) with its vertical part removed. A display
    // lying flat (z ~ up) has no usable normal: use the screen's top edge (y).
    float axis[3] = {0, 0, m->flip};
    if (fabsf(dot(axis, m->up)) > 0.9f) {
        axis[0] = 0; axis[1] = m->flip; axis[2] = 0;
    }
    float d = dot(axis, m->up);
    for (int i = 0; i < 3; i++) m->fwd[i] = axis[i] - d * m->up[i];
    normalize(m->fwd);
    cross(m->fwd, m->up, m->right);
    normalize(m->right);
    m->have_frame = true;
}

void motion_init(motion_est_t *m, float flip)
{
    memset(m, 0, sizeof(*m));
    m->flip = flip < 0 ? -1.0f : 1.0f;
}

void motion_set_up(motion_est_t *m, const float up[3], bool calibrated)
{
    memcpy(m->up, up, sizeof(m->up));
    if (!normalize(m->up)) return;
    m->calibrated = calibrated;
    build_frame(m);
}

void motion_zero(motion_est_t *m, const float accel[3])
{
    motion_set_up(m, accel, true);
    m->lean = 0;
}

// While riding, the specific force points along the bike's own vertical (in
// turns too), so its slow average is the bike's up in sensor axes.
static void learn_up(motion_est_t *m, const float a[3], float dt)
{
    float n = sqrtf(dot(a, a));
    if (n < 1e-3f || fabsf(n / G - 1.0f) > UP_GATE) {
        return;
    }
    float k = dt / ((m->calibrated && m->shaken_s <= 0 ? UP_TAU : UP_TAU_NEW) + dt);
    for (int i = 0; i < 3; i++) m->up[i] += (a[i] / n - m->up[i]) * k;
    normalize(m->up);
    build_frame(m);
    m->learn_s += dt;
    if (m->learn_s >= UP_SETTLE) m->calibrated = true;
}

static void learn_bias(motion_est_t *m, const float a[3], const float gyro_dps[3], float dt)
{
    float n = sqrtf(dot(a, a));
    if (fabsf(n / G - 1.0f) > BIAS_ACCEL_GATE) return;
    for (int i = 0; i < 3; i++) {
        if (fabsf(gyro_dps[i]) > BIAS_GATE) return;
    }
    // A running mean at first, then a slow average that follows the offset's
    // drift (it moves with temperature).
    float k = dt / ((m->bias_s < BIAS_TAU ? m->bias_s : BIAS_TAU) + dt);
    for (int i = 0; i < 3; i++) m->gyro_bias[i] += (gyro_dps[i] - m->gyro_bias[i]) * k;
    m->bias_s += dt;
}

void motion_update(motion_est_t *m, const float a[3], const float gyro_raw[3], float dt, float kmh)
{
    if (kmh >= 0 && kmh < STILL_KMH) learn_bias(m, a, gyro_raw, dt);
    float gyro_dps[3];
    for (int i = 0; i < 3; i++) gyro_dps[i] = gyro_raw[i] - m->gyro_bias[i];
    if (!m->have_frame) {
        for (int i = 0; i < 3; i++) m->boot_sum[i] += a[i];
        if (++m->boot_n >= BOOT_N) {
            motion_set_up(m, m->boot_sum, false);
        }
        return;
    }
    bool was_valid = m->valid;
    m->valid = kmh >= (was_valid ? LEAN_OFF_KMH : LEAN_ON_KMH);
    float au = dot(a, m->up), af = dot(a, m->fwd);
    m->pitch = atan2f(af, au) * DEG;
    if (!m->valid) {
        // No lean standing still or crawling: on the side stand, or with the
        // board moving in its case, the tilt isn't the bike's.
        m->lean = m->lat_g = m->lon_g = 0;
        return;
    }
    if (sqrtf(dot(gyro_dps, gyro_dps)) > JOLT_DPS) {
        m->shaken_s = SHAKEN_S;   // a jolt: this sample says nothing about the bike
        return;
    }
    bool shaken = m->shaken_s > 0;
    if (shaken) m->shaken_s -= dt;
    learn_up(m, a, dt);
    float w[3] = {gyro_dps[0] * RAD, gyro_dps[1] * RAD, gyro_dps[2] * RAD};
    af = dot(a, m->fwd);
    float roll_rate = dot(w, m->fwd) * DEG;   // + rolling right (right-hand rule about forward)

    // Yaw about the true vertical shows on the bike's up and right axes (cos
    // and sin of the lean): the true vertical is cos(lean) up - sin(lean)
    // right, so the yaw is the gyro along it. (A magnitude of the two took in
    // the pitch rate too, and made any offset a turn.) Turning right is
    // clockwise from above: negative about up.
    float v = kmh / 3.6f;
    float wu = dot(w, m->up), wr = dot(w, m->right);
    float yaw = wu * cosf(m->lean * RAD) - wr * sinf(m->lean * RAD);
    float lat = -v * yaw / G;   // + right
    if (lat > LAT_MAX) lat = LAT_MAX;
    if (lat < -LAT_MAX) lat = -LAT_MAX;
    float turn = atanf(lat) * DEG;
    if (!was_valid) {
        m->lean = turn;   // a fresh start, not whatever was there when it stopped
        m->lat_g = lat;
        m->lon_g = af / G;
    }
    if (shaken) {
        m->lean = SHAKEN_KEEP * m->lean + (1 - SHAKEN_KEEP) * turn;
    } else {
        m->lean = ROLL_KEEP * (m->lean + roll_rate * dt) + (1 - ROLL_KEEP) * turn;
    }
    m->lat_g += (lat - m->lat_g) * G_SMOOTH;
    m->lon_g += (af / G - m->lon_g) * G_SMOOTH;
}
