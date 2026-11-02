/*
 * Demo mode: no Bluetooth. Plays a synthetic five-minute ride (pulling away,
 * town, a fast road, a roundabout, traffic lights) geared to the bike profile,
 * so the rev counter, the gear badge and every other page move as they would
 * on the bike. Each loop also stages what a desk can't produce: overheating
 * (236 s), a fault code with the MIL (260 s), and the engine stopped
 * (285-300 s) so the clear-codes flow can be tried. Pure C, host-tested.
 */
#pragma once

#include <stdint.h>
#include "obd/obd_state.h"

void demo_start(obd_state_t *s, int64_t now);
void demo_feed(obd_state_t *s, int64_t now);
// The rider asked to clear codes: refused while the engine runs, like the bike.
void demo_request_clear(obd_state_t *s, int64_t now);
// The gear the demo is riding in (1-based), 0 when stopped or the engine is off.
int demo_gear(void);
