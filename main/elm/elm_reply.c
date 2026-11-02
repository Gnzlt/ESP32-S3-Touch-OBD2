#include <string.h>
#include "elm_reply.h"

#define CR     0x0D
#define LF     0x0A
#define SPACE  0x20
#define COLON  0x3A
#define PROMPT 0x3E

int hex_nibble(uint8_t c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

void elm_assembler_reset(elm_assembler_t *a)
{
    a->len = 0;
}

void elm_assembler_feed(elm_assembler_t *a, const uint8_t *data, size_t len,
                        void (*on_reply)(void *ctx, const uint8_t *reply, int len), void *ctx)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];
        if (b == PROMPT) {
            int n = a->len;
            a->len = 0;
            on_reply(ctx, a->buf, n);
        } else if (b != 0 && a->len < ELM_REPLY_MAX) {
            // Some clones send NUL padding; it is never part of a reply.
            a->buf[a->len++] = b;
        }
    }
}

// Hex pairs, spaces allowed between them, into out. -1 if not hex or odd.
static int hex_decode(const uint8_t *raw, int s, int e, uint8_t *out, int cap)
{
    int n = 0;
    int hi = -1;
    for (int i = s; i < e; i++) {
        if (raw[i] == SPACE) {
            continue;
        }
        int v = hex_nibble(raw[i]);
        if (v < 0) {
            return -1;
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (n < cap) {
                out[n] = (uint8_t)((hi << 4) | v);
            }
            n++;
            hi = -1;
        }
    }
    if (hi >= 0 || n == 0) {
        return -1;
    }
    return n < cap ? n : cap;
}

static bool all_hex(const uint8_t *raw, int s, int e)
{
    for (int i = s; i < e; i++) {
        if (hex_nibble(raw[i]) < 0) return false;
    }
    return true;
}

static bool starts_with(const uint8_t *raw, int s, int e, const char *word)
{
    int n = (int)strlen(word);
    return e - s >= n && memcmp(raw + s, word, n) == 0;
}

// "SEARCHING..." and "BUS INIT: ...": progress lines, not part of the answer.
static bool is_progress(const uint8_t *raw, int s, int e)
{
    return starts_with(raw, s, e, "SEARCHING") || starts_with(raw, s, e, "BUS INIT");
}

// The status a line reports, or ELM_OK for anything else. Switches on the
// first byte so data lines skip every keyword comparison.
static elm_status_t error_of(const uint8_t *raw, int s, int e)
{
    switch (raw[s]) {
    case '?': return e - s == 1 ? ELM_UNKNOWN_CMD : ELM_OK;
    case 'N': return starts_with(raw, s, e, "NO DATA") ? ELM_NO_DATA : ELM_OK;
    case 'U': return starts_with(raw, s, e, "UNABLE TO CONNECT") ? ELM_UNABLE : ELM_OK;
    case 'C': return starts_with(raw, s, e, "CAN ERROR") ? ELM_CAN_ERROR : ELM_OK;
    case 'S': return starts_with(raw, s, e, "STOPPED") ? ELM_STOPPED : ELM_OK;
    case 'B': return starts_with(raw, s, e, "BUS ") || starts_with(raw, s, e, "BUFFER FULL") ? ELM_BUS_ERROR : ELM_OK;
    case 'F': return starts_with(raw, s, e, "FB ERROR") ? ELM_BUS_ERROR : ELM_OK;
    case 'D': return starts_with(raw, s, e, "DATA ERROR") ? ELM_BUS_ERROR : ELM_OK;
    case '<': return starts_with(raw, s, e, "<RX ERROR") ? ELM_BUS_ERROR : ELM_OK;
    case 'E': return starts_with(raw, s, e, "ERR") ? ELM_BUS_ERROR : ELM_OK;
    }
    return ELM_OK;
}

static void add_msg(elm_result_t *r, const uint8_t *bytes, int len)
{
    if (r->n_msgs >= ELM_MAX_MSGS || len <= 0) {
        return;
    }
    if (len > ELM_MAX_MSG) {
        len = ELM_MAX_MSG;
    }
    memcpy(r->msg[r->n_msgs], bytes, len);
    r->msg_len[r->n_msgs] = len;
    r->n_msgs++;
}

void elm_parse(const uint8_t *raw, int n, bool obd, elm_result_t *r)
{
    r->status = ELM_EMPTY;
    r->n_msgs = 0;
    r->text[0] = 0;
    int text_s = 0, text_e = 0;
    // Multi-frame CAN: "00E" length line, then "0:..." "1:..." frames.
    uint8_t frame[ELM_MAX_MSG];
    int frame_len = 0;      // bytes collected
    int frame_total = -1;   // announced length; -1 = no frame open
    uint8_t tmp[ELM_MAX_MSG];

    int s = 0;
    while (s < n) {
        int e = s;
        while (e < n && raw[e] != CR && raw[e] != LF) e++;
        int next = e + 1;
        while (s < e && raw[s] == SPACE) s++;
        while (e > s && raw[e - 1] == SPACE) e--;

        if (e > s && !is_progress(raw, s, e)) {
            text_s = s;
            text_e = e;
            elm_status_t err = error_of(raw, s, e);
            if (err != ELM_OK) {
                if (r->status == ELM_EMPTY) r->status = err;
            } else if (obd) {
                if (e - s == 3 && all_hex(raw, s, e)) {
                    // ISO-TP length line: three hex digits.
                    frame_total = (hex_nibble(raw[s]) << 8) | (hex_nibble(raw[s + 1]) << 4) | hex_nibble(raw[s + 2]);
                    frame_len = 0;
                } else if (frame_total >= 0 && e - s > 2 && raw[s + 1] == COLON && hex_nibble(raw[s]) >= 0) {
                    int m = hex_decode(raw, s + 2, e, tmp, sizeof(tmp));
                    if (m > 0) {
                        for (int i = 0; i < m && frame_len < ELM_MAX_MSG; i++) frame[frame_len++] = tmp[i];
                        if (frame_len >= frame_total || frame_len >= ELM_MAX_MSG) {
                            add_msg(r, frame, frame_total < frame_len ? frame_total : frame_len);
                            frame_total = -1;
                            frame_len = 0;
                        }
                    } else if (r->status == ELM_EMPTY) {
                        r->status = ELM_OTHER_ERROR;
                    }
                } else {
                    int m = hex_decode(raw, s, e, tmp, sizeof(tmp));
                    if (m > 0) {
                        add_msg(r, tmp, m);
                    } else if (r->status == ELM_EMPTY) {
                        r->status = ELM_OTHER_ERROR;
                    }
                }
            } else {
                int m = hex_decode(raw, s, e, tmp, sizeof(tmp));
                if (m > 0) add_msg(r, tmp, m);
                if (r->status == ELM_EMPTY) r->status = ELM_OK;
            }
        }
        s = next;
    }
    // A multi-frame message cut short still carries whole PIDs at its start.
    if (frame_total >= 0 && frame_len > 0) {
        add_msg(r, frame, frame_len);
    }
    if (r->n_msgs > 0) {
        r->status = ELM_OK;
    }
    int tl = text_e - text_s;
    if (tl > ELM_TEXT_LEN - 1) tl = ELM_TEXT_LEN - 1;
    if (tl > 0) memcpy(r->text, raw + text_s, tl);
    r->text[tl > 0 ? tl : 0] = 0;
}
