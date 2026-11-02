/*
 * Honda CB500X, 2022 (471 cc parallel twin, six speeds, Euro 5). The bike the
 * dashboard was developed and field-tested on. Its ECU speaks ISO 15765-4 CAN
 * 29-bit 500k (ELM protocol 7) and supports Mode 01 PIDs 02 03 04 05 06 0B 0C
 * 0D 0E 0F 11 12 1C 21 41: no MAF, no fuel level, no PID 01.
 */
#pragma once

#define BIKE_NAME            "CB500X"
#define BIKE_RPM_MAX         10000.0f   // gauge end
#define BIKE_RPM_RED         8500.0f    // red zone start
#define BIKE_RPM_IDLE        1200.0f    // warm idle, measured: 1,205 rpm

#define BIKE_COOLANT_COLD    60.0f
#define BIKE_COOLANT_WARN    100.0f
#define BIKE_COOLANT_ALERT   105.0f     // a hot day in traffic peaked at 100

// Gear ratios (Honda spec). K was fitted from a 48-minute ride log: rpm per
// km/h clustered at 102.5, 77.5, 62.8 and 55.5 in 2nd to 5th. It folds in the
// primary and final drive, the rear tyre and the speed PID's own error.
#define BIKE_GEAR_RATIOS     {3.285f, 2.105f, 1.600f, 1.300f, 1.150f, 1.043f}
#define BIKE_GEAR_K          48.3f

#define BIKE_DISPLACEMENT_L  0.471f
#define BIKE_TANK_L          17.5f      // sources say 17.5 or 17.7: the lower, for a cautious range

// Cold tyre pressures, gauge kPa (owner's manual: front 200, rear 225; check
// the label on the chain guard).
#define BIKE_TYRE_FRONT_KPA       200.0f
#define BIKE_TYRE_REAR_KPA        225.0f
#define BIKE_TYRE_LOW_FRONT_KPA   180.0f
#define BIKE_TYRE_LOW_REAR_KPA    200.0f
