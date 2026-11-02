/*
 * The motorcycle's numbers, in one place. They come from a profile header in
 * bike/profiles/, picked in menuconfig (OBD2 dashboard -> Bike profile) and
 * passed in by main/CMakeLists.txt as BIKE_PROFILE_H. To add a bike, copy
 * profiles/generic.h to profiles/<your_bike>.h, fill it in and select it.
 * docs/configure-your-bike.md explains each value and how to find it.
 *
 * Optional values get their defaults here, so a profile can leave them out.
 */
#pragma once

#ifdef BIKE_PROFILE_H
#include BIKE_PROFILE_H
#else
#include "profiles/generic.h"
#endif

// ---- Required ----------------------------------------------------------------

#if !defined(BIKE_NAME) || !defined(BIKE_RPM_MAX) || !defined(BIKE_RPM_RED)
#error "bike profile: BIKE_NAME, BIKE_RPM_MAX and BIKE_RPM_RED are required"
#endif
#if !defined(BIKE_DISPLACEMENT_L) || !defined(BIKE_TANK_L)
#error "bike profile: BIKE_DISPLACEMENT_L and BIKE_TANK_L are required"
#endif
#if !defined(BIKE_TYRE_FRONT_KPA) || !defined(BIKE_TYRE_REAR_KPA)
#error "bike profile: BIKE_TYRE_FRONT_KPA and BIKE_TYRE_REAR_KPA are required"
#endif

// ---- Optional, with defaults --------------------------------------------------

#ifndef BIKE_RPM_IDLE
#define BIKE_RPM_IDLE        1300.0f    // warm idle; the demo ride idles here
#endif

// Coolant, °C. Bikes without a coolant PID (most air-cooled ones) just show --.
#ifndef BIKE_COOLANT_COLD
#define BIKE_COOLANT_COLD    60.0f      // blue below this: still warming up
#endif
#ifndef BIKE_COOLANT_WARN
#define BIKE_COOLANT_WARN    100.0f     // amber
#endif
#ifndef BIKE_COOLANT_ALERT
#define BIKE_COOLANT_ALERT   105.0f     // red + alert
#endif

// Battery voltage (the adapter's ATRV), for a 12 V system.
#ifndef BIKE_VOLT_LOW
#define BIKE_VOLT_LOW        11.8f      // with the engine running: not charging
#endif
#ifndef BIKE_VOLT_HIGH
#define BIKE_VOLT_HIGH       15.2f      // regulator fault
#endif
#ifndef BIKE_VOLT_CHARGING
#define BIKE_VOLT_CHARGING   13.2f      // above this the alternator is charging
#endif

// Gear estimate (obd/gear.c). Without BIKE_GEAR_RATIOS there is no estimate
// and the Ride page hides its gear badge. BIKE_GEAR_K is rpm per km/h divided
// by the gear ratio: give it directly (fitted from a log with
// tools/fit_gears.py), or let it follow from the primary and final drive and
// the rear tyre's rolling circumference.
#if defined(BIKE_GEAR_RATIOS) && !defined(BIKE_GEAR_K)
#if !defined(BIKE_PRIMARY_RATIO) || !defined(BIKE_FINAL_RATIO) || !defined(BIKE_REAR_TYRE_CIRC_M)
#error "bike profile: with BIKE_GEAR_RATIOS, give BIKE_GEAR_K or BIKE_PRIMARY_RATIO, BIKE_FINAL_RATIO and BIKE_REAR_TYRE_CIRC_M"
#endif
// km/h -> wheel rpm is 1000 / 60 / circumference; the engine turns primary x
// gear x final times as fast.
#define BIKE_GEAR_K          (BIKE_PRIMARY_RATIO * BIKE_FINAL_RATIO * 1000.0f / 60.0f / BIKE_REAR_TYRE_CIRC_M)
#endif
#ifndef BIKE_GEAR_TOLERANCE
#define BIKE_GEAR_TOLERANCE  0.045f     // a ratio within 4.5 % of a gear's is that gear
#endif
#ifndef BIKE_GEAR_MIN_KMH
#define BIKE_GEAR_MIN_KMH    8.0f       // below this the speed PID is too coarse (1 km/h steps)
#endif

// Fuel estimate (obd/fuel.c): the wide-open volumetric efficiency. 0.90 is a
// first guess for a modern 4-valve engine; calibrating from fill-ups absorbs
// its error.
#ifndef BIKE_VE_PEAK
#define BIKE_VE_PEAK         0.90f
#endif

// The default "warn me below" tyre pressures, about 10 % under the cold
// pressures. The rider changes them on the Tyres page.
#ifndef BIKE_TYRE_LOW_FRONT_KPA
#define BIKE_TYRE_LOW_FRONT_KPA   (BIKE_TYRE_FRONT_KPA * 0.9f)
#endif
#ifndef BIKE_TYRE_LOW_REAR_KPA
#define BIKE_TYRE_LOW_REAR_KPA    (BIKE_TYRE_REAR_KPA * 0.9f)
#endif
