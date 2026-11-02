/*
 * A generic motorcycle: safe defaults for any bike, and the template for your
 * own profile. Copy this file to profiles/<your_bike>.h, change the numbers,
 * and pick it in menuconfig (OBD2 dashboard -> Bike profile: "<your_bike>").
 * docs/configure-your-bike.md explains every value; bike/bike.h lists the
 * optional ones and their defaults.
 */
#pragma once

// Shown on the boot splash (keep it short: about 12 characters fit).
#define BIKE_NAME            "OBD2 DASH"

// The rev counter on the Ride page. BIKE_RPM_MAX is the gauge's end (a whole
// thousand); the red zone starts at BIKE_RPM_RED.
#define BIKE_RPM_MAX         12000.0f
#define BIKE_RPM_RED         10000.0f
#define BIKE_RPM_IDLE        1300.0f

// Gear estimate: OBD2 has no gear PID, so the gear comes from rpm / speed.
// Leave BIKE_GEAR_RATIOS out and the Ride page shows no gear. With it, give
// either BIKE_GEAR_K, or the primary and final drive and the rear tyre's
// rolling circumference (metres). For example (made-up numbers):
//
// #define BIKE_GEAR_RATIOS       {2.846f, 1.947f, 1.556f, 1.333f, 1.190f, 1.083f}
// #define BIKE_PRIMARY_RATIO     1.822f
// #define BIKE_FINAL_RATIO       (45.0f / 16.0f)
// #define BIKE_REAR_TYRE_CIRC_M  1.94f

// Fuel estimate and range: engine displacement in litres and tank size.
#define BIKE_DISPLACEMENT_L  0.65f
#define BIKE_TANK_L          15.0f

// Cold tyre pressures, gauge kPa (2.25 bar = 225 kPa = 32.6 psi). The low
// alerts default to 10 % under them.
#define BIKE_TYRE_FRONT_KPA  225.0f
#define BIKE_TYRE_REAR_KPA   250.0f
