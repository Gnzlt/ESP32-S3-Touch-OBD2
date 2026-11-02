/*
 * Turns the ELM327's notification fragments into whole replies, and whole
 * replies into OBD messages or a status. A reply ends at the '>' prompt.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    ELM_OK,
    ELM_EMPTY,        // nothing but blank lines / SEARCHING...
    ELM_NO_DATA,      // the ECU did not answer this request
    ELM_UNABLE,       // UNABLE TO CONNECT: no ECU on any protocol (ignition off)
    ELM_CAN_ERROR,
    ELM_STOPPED,      // a new byte arrived while the ELM was waiting
    ELM_UNKNOWN_CMD,  // "?"
    ELM_BUS_ERROR,    // BUS ERROR, BUS BUSY, FB ERROR, DATA ERROR, BUFFER FULL, <RX ERROR
    ELM_OTHER_ERROR,  // unparseable line in OBD mode
} elm_status_t;

#define ELM_MAX_MSGS   8
#define ELM_MAX_MSG    64
#define ELM_TEXT_LEN   40
#define ELM_REPLY_MAX  512

typedef struct {
    elm_status_t status;
    int n_msgs;
    uint8_t msg[ELM_MAX_MSGS][ELM_MAX_MSG];   // OBD mode: every message as bytes (multi-frame joined)
    int msg_len[ELM_MAX_MSGS];
    char text[ELM_TEXT_LEN];                  // last non-empty line: "OK", "A7", "13.8V"...
} elm_result_t;

// Collects notification bytes until the prompt.
typedef struct {
    uint8_t buf[ELM_REPLY_MAX];
    int len;
} elm_assembler_t;

void elm_assembler_reset(elm_assembler_t *a);

// Feed bytes. Calls on_reply(ctx, reply, len) for each reply (the bytes
// before '>') they complete. A reply longer than the buffer is cut off; the
// parser then sees a truncated line, never a crash.
void elm_assembler_feed(elm_assembler_t *a, const uint8_t *data, size_t len,
                        void (*on_reply)(void *ctx, const uint8_t *reply, int len), void *ctx);

// Parse one reply. With obd = true a line that is neither hex nor a known
// status is an error, and CAN multi-frame replies are joined; with obd = false
// (AT commands) only the status and the text matter.
void elm_parse(const uint8_t *raw, int n, bool obd, elm_result_t *out);

int hex_nibble(uint8_t c);
