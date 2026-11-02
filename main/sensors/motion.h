/*
 * Lean angle and G from the IMU, only while riding (valid from 15 km/h, until
 * below 10 km/h). An accelerometer alone reads ~0° in a steady turn (the bike
 * leans so the net force stays along its own vertical), so the lean comes
 * from the turn itself: tan(lean) = v * yaw_rate / g, with v from the speed
 * PID and the yaw rate from the gyro, fused with the gyro's roll rate for
 * quick response. Standing still there is no lean: on the side stand, or with
 * the board moving in a loose case, the tilt says nothing about riding.
 *
 * Frame: "up" is the bike's vertical in sensor axes. That same fact (the net
 * force follows the bike's vertical) means the accelerometer averaged while
 * riding *is* the bike's up, however the board sits, so it is learned
 * continuously; a zero or the boot guess only seeds it. "Forward" is the
 * screen's normal made horizontal (the display faces the rider), "right" =
 * forward x up. Lean is + to the right.
 *
 * A board loose in a bag gets jolted: a turn or roll faster than 200 deg/s is
 * skipped, lateral g is capped at 1.2, and for 3 s after a jolt up is learned
 * afresh and the lean follows the turn alone.
 *
 * The gyro's zero-rate offset is learned whenever the bike stands still (speed
 * 0) and subtracted: uncorrected, it reads as a steady turn (a ride leaned ~10
 * degrees left on straights). Pure C, host-testable.
 */
#pragma once

#include <stdbool.h>

typedef struct {
    float up[3], fwd[3], right[3];
    bool have_frame;
    bool calibrated;             // up came from a zero or from riding, not the boot guess
    bool valid;                  // riding fast enough: lean and G mean something
    float learn_s;               // seconds of up-learning while riding
    float shaken_s;              // seconds left of re-settling after a jolt (the board moved in its case)
    float boot_sum[3];
    int boot_n;
    float lean, lat_g, lon_g, pitch;
    float gyro_bias[3];          // deg/s: the gyro's zero-rate offset, learned standing still
    float bias_s;                // seconds it has been learned for; 0 = not yet
    float flip;                  // +1, or -1 if the IMU's z axis points into the screen
} motion_est_t;

void motion_init(motion_est_t *m, float flip);
// A saved zero (unit "up" vector in sensor axes), or NULL to guess at boot.
void motion_set_up(motion_est_t *m, const float up[3], bool calibrated);
// accel in m/s^2 (specific force), gyro in deg/s (raw: the offset is taken off
// here), dt in s. kmh < 0 = unknown.
void motion_update(motion_est_t *m, const float accel[3], const float gyro[3], float dt, float kmh);
// Zero on the current gravity: the bike upright, standing still.
void motion_zero(motion_est_t *m, const float accel[3]);
