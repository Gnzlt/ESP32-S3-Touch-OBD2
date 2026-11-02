/*
 * The tyre sensors' configuration as a text file on the microSD card
 * (/obd2/tpms.cfg): which sensor is on which tyre, and every alert limit.
 * The dashboard writes it whenever either changes and reads it at each mount,
 * so a card that carries it sets up a freshly flashed or different board.
 * One "key = value" per line; '#' starts a comment; unknown keys are ignored,
 * and a key that is missing keeps its default. Pure C.
 *
 *   front_format = DJTPMS
 *   front_id = 3CAB72445526          (the payload id, hex; empty without one)
 *   front_addr = 26:55:44:72:AB:3C   (the BLE address, as usually printed)
 *   front_low_alert = on
 *   front_low_kpa = 180
 *   ...
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "tpms_alerts.h"
#include "tpms_pair.h"

// Returns the length written (truncated to n - 1).
int tpms_cfgfile_write(char *out, size_t n, const tpms_pairing_t pairs[TPMS_TYRES], const tpms_cfg_t *c);
// Starts from no sensors and tpms_cfg_defaults(). False when the text holds
// no key this file knows (empty, or not this file): nothing should change.
bool tpms_cfgfile_parse(const char *text, tpms_pairing_t pairs[TPMS_TYRES], tpms_cfg_t *c);
