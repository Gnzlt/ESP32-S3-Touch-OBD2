/*
 * Mode 03 (stored) and Mode 07 (pending) fault codes: two bytes per code,
 * rendered as the usual P0123 form, plus short descriptions of the generic
 * codes a motorcycle is likely to set.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "obd_state.h"
#include "elm/elm_reply.h"

#define DTC_MODE_STORED  0x03
#define DTC_MODE_CLEAR   0x04
#define DTC_MODE_PENDING 0x07

// Codes in every message of one reply into out (deduplicated). On CAN
// (ISO 15765) the byte after the mode is the code count; older protocols pad
// with 00 00 instead. Returns the number of codes.
int dtc_decode_reply(const elm_result_t *r, int mode, bool is_can, char out[DTC_MAX][DTC_LEN]);

// A: 2 bits system (P C B U), 2 bits first digit, 4 bits second; B: last two.
void dtc_format(uint8_t a, uint8_t b, char out[DTC_LEN]);

// A short English description, or a description of the code's group.
const char *dtc_describe(const char *code);
