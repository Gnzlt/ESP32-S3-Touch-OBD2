#include <stdio.h>
#include <string.h>
#include "obd_state.h"

// How old a value may be and still count. Values are cleared at once when the
// link drops or the ECU goes quiet, so this only bridges a few lost replies.
// RPM, speed and throttle are read many times a second here, so after 1.5 s a
// held value would be a lie; the slow PIDs are read every 1-2 s.
#define STALE_MS       6000
#define STALE_FAST_MS  1500
#define STALE_INFO_MS  90000    // 1C, 21, 41: read every 30 s

void obd_state_init(obd_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->phase = PHASE_SCAN;
    s->last_reply_at = -1;
    for (int i = 0; i <= PID_MAX; i++) {
        s->at[i] = -1;
    }
}

void obd_state_set(obd_state_t *s, uint8_t pid, float value, int64_t now)
{
    if (pid > PID_MAX) {
        return;
    }
    s->val[pid] = value;
    s->at[pid] = now;
}

bool obd_fresh(const obd_state_t *s, uint8_t pid, int64_t now, int max_age_ms, float *out)
{
    if (pid > PID_MAX) {
        return false;
    }
    int64_t at = s->at[pid];
    if (at < 0 || now - at > max_age_ms) {
        return false;
    }
    if (out) {
        *out = s->val[pid];
    }
    return true;
}

bool obd_value(const obd_state_t *s, uint8_t pid, int64_t now, float *out)
{
    int age = STALE_MS;
    if (pid == PID_RPM || pid == PID_SPEED || pid == PID_THROTTLE) {
        age = STALE_FAST_MS;
    } else if (pid == PID_OBD_STD || pid == PID_MIL_DIST || pid == PID_MONITOR || pid == PID_SEC_AIR) {
        age = STALE_INFO_MS;
    }
    return obd_fresh(s, pid, now, age, out);
}

void obd_forget_values(obd_state_t *s)
{
    for (int i = 0; i <= PID_MAX; i++) {
        s->at[i] = -1;
    }
}

void obd_clear_support(obd_state_t *s)
{
    memset(s->supported, 0, sizeof(s->supported));
    s->support_known = false;
}

bool obd_is_supported(const obd_state_t *s, uint8_t pid)
{
    return pid <= PID_MAX && s->supported[pid];
}

// Bit 31 of the 4 bitmap bytes is PID base + 1. ORed in, not assigned: when
// the ABS unit answers 0100 too, its reply must not erase the engine's PIDs.
static void apply_bitmap(obd_state_t *s, uint8_t base, const uint8_t *d)
{
    for (int i = 0; i < 32; i++) {
        int pid = base + i + 1;
        if (pid > PID_MAX) {
            break;
        }
        if ((d[i / 8] >> (7 - i % 8)) & 1) {
            s->supported[pid] = true;
        }
    }
}

void obd_support_hex(const obd_state_t *s, uint8_t base, char out[9])
{
    for (int b = 0; b < 4; b++) {
        int v = 0;
        for (int i = 0; i < 8; i++) {
            int pid = base + b * 8 + i + 1;
            if (pid <= PID_MAX && s->supported[pid]) {
                v |= 1 << (7 - i);
            }
        }
        snprintf(out + b * 2, 3, "%02X", v);
    }
}

int obd_apply_mode01(obd_state_t *s, const uint8_t *msg, int len, int64_t now)
{
    if (len < 2 || msg[0] != 0x41) {
        return 0;
    }
    int count = 0;
    int i = 1;
    while (i < len) {
        uint8_t pid = msg[i];
        int n = pid_data_length(pid);
        if (n < 0 || i + 1 + n > len) {
            break;
        }
        const uint8_t *d = msg + i + 1;
        if (pid_is_bitmap(pid)) {
            // Bitmaps are asked for alone. One further in is a misread length
            // (a 2-byte trim read as one), and must not rewrite what is supported.
            if (i != 1) break;
            apply_bitmap(s, pid, d);
        } else if (pid == PID_STATUS) {
            s->mil = (d[0] & 0x80) != 0;
            s->dtc_count = d[0] & 0x7F;
        } else {
            obd_state_set(s, pid, pid_decode(pid, d), now);
            memcpy(s->raw[pid], d, n);
        }
        count++;
        i += 1 + n;
    }
    return count;
}

// Printable ASCII only, padding (NUL, spaces) trimmed from the end.
static void copy_text(char *out, size_t size, const uint8_t *in, int n)
{
    int k = 0;
    for (int i = 0; i < n && k < (int)size - 1; i++) {
        out[k++] = in[i] >= 0x20 && in[i] < 0x7F ? (char)in[i] : ' ';
    }
    while (k > 0 && out[k - 1] == ' ') k--;
    out[k] = 0;
}

bool obd_apply_mode09(obd_state_t *s, const uint8_t *msg, int len)
{
    if (len < 3 || msg[0] != 0x49) {
        return false;
    }
    // After "49 pid" comes a count byte (ISO 15765), then the items.
    int count = msg[2];
    const uint8_t *d = msg + 3;
    int n = len - 3;
    switch (msg[1]) {
    case 0x02:
        if (n < 17) return false;
        copy_text(s->vin, sizeof(s->vin), d + n - 17, 17);
        return true;
    case 0x04:
        for (int i = 0; i < 2; i++) {
            if (i < count && n >= 16 * (i + 1)) {
                copy_text(s->cal_id[i], sizeof(s->cal_id[i]), d + 16 * i, 16);
            } else {
                s->cal_id[i][0] = 0;
            }
        }
        return n >= 16;
    case 0x06:
        for (int i = 0; i < 2; i++) {
            if (i < count && n >= 4 * (i + 1)) {
                snprintf(s->cvn[i], sizeof(s->cvn[i]), "%02X%02X%02X%02X", d[4 * i], d[4 * i + 1], d[4 * i + 2],
                         d[4 * i + 3]);
            } else {
                s->cvn[i][0] = 0;
            }
        }
        return n >= 4;
    }
    return false;
}

void obd_set_error(obd_state_t *s, const char *text)
{
    snprintf(s->last_error, sizeof(s->last_error), "%s", text);
}
