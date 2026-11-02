#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "tpms_cfgfile.h"

static const char *const TYRE[TPMS_TYRES] = {"front", "rear"};

static int put(char *out, size_t n, int len, const char *fmt, ...) __attribute__((format(printf, 4, 5)));

static int put(char *out, size_t n, int len, const char *fmt, ...)
{
    if (len >= (int)n - 1) return len;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(out + len, n - len, fmt, ap);
    va_end(ap);
    len += k;
    return len < (int)n ? len : (int)n - 1;
}

static const char *onoff(bool b) { return b ? "on" : "off"; }

int tpms_cfgfile_write(char *out, size_t n, const tpms_pairing_t pairs[TPMS_TYRES], const tpms_cfg_t *c)
{
    int len = 0;
    out[0] = 0;
    len = put(out, n, len,
              "# Tyre sensors (TPMS). The dashboard writes this file when a pairing or a limit\n"
              "# changes, and reads it whenever the card mounts: with this card in, any board listens\n"
              "# for these sensors. Pressures are gauge kPa (2.00 bar = 200), temperatures deg C.\n\n");
    for (int t = 0; t < TPMS_TYRES; t++) {
        const tpms_pairing_t *p = &pairs[t];
        bool paired = p->format > TPMS_F_NONE && p->format < TPMS_F_COUNT;
        char id[2 * TPMS_ID_MAX + 1] = "";
        for (int i = 0; paired && i < p->id_len && i < TPMS_ID_MAX; i++) sprintf(id + 2 * i, "%02X", p->id[i]);
        len = put(out, n, len, "%s_format = %s\n", TYRE[t], paired ? TPMS_FORMAT_NAMES[p->format] : "none");
        len = put(out, n, len, "%s_id = %s\n", TYRE[t], id);
        if (paired) {
            len = put(out, n, len, "%s_addr = %02X:%02X:%02X:%02X:%02X:%02X\n", TYRE[t], p->addr[5], p->addr[4],
                      p->addr[3], p->addr[2], p->addr[1], p->addr[0]);
        } else {
            len = put(out, n, len, "%s_addr =\n", TYRE[t]);
        }
        len = put(out, n, len, "%s_low_alert = %s\n%s_low_kpa = %.0f\n", TYRE[t], onoff(c->low_on[t]), TYRE[t],
                  c->low_kpa[t]);
        len = put(out, n, len, "%s_high_alert = %s\n%s_high_kpa = %.0f\n\n", TYRE[t], onoff(c->high_on[t]), TYRE[t],
                  c->high_kpa[t]);
    }
    len = put(out, n, len, "alerts = %s\nhot_alert = %s\nhot_c = %.0f\ncold_alert = %s\ncold_c = %.0f\n",
              onoff(c->enabled), onoff(c->hot_on), c->hot_c, onoff(c->cold_on), c->cold_c);
    len = put(out, n, len, "fast_loss_pct = %d\nsensor_alarm = %s\nbattery_alert = %s\nsilent_min = %d\n", c->loss_pct,
              onoff(c->sensor_alarm), onoff(c->battery), c->silent_min);
    return len;
}

static int hexval(int ch)
{
    ch = toupper(ch);
    return ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

// Hex digits into bytes, skipping ':' and spaces; returns the byte count, or -1.
static int hexbytes(const char *s, uint8_t *out, int max)
{
    int n = 0, high = -1;
    for (; *s; s++) {
        if (*s == ':' || *s == ' ' || *s == '-') continue;
        int v = hexval((unsigned char)*s);
        if (v < 0 || n >= max) return -1;
        if (high < 0) {
            high = v;
        } else {
            out[n++] = (uint8_t)(high * 16 + v);
            high = -1;
        }
    }
    return high < 0 ? n : -1;
}

static bool parse_bool(const char *v, bool *out)
{
    if (!strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1")) {
        return *out = true, true;
    }
    if (!strcasecmp(v, "off") || !strcasecmp(v, "no") || !strcasecmp(v, "false") || !strcmp(v, "0")) {
        return *out = false, true;
    }
    return false;
}

static bool parse_num(const char *v, float *out)
{
    char *end;
    float f = strtof(v, &end);
    if (end == v || *end) return false;
    *out = f;
    return true;
}

static bool apply_tyre(const char *k, const char *v, tpms_pairing_t *p, tpms_cfg_t *c, int t)
{
    float f;
    if (!strcmp(k, "format")) {
        p->format = TPMS_F_NONE;
        for (int i = 1; i < TPMS_F_COUNT; i++) {
            if (!strcasecmp(v, TPMS_FORMAT_NAMES[i])) p->format = (uint8_t)i;
        }
        return true;
    }
    if (!strcmp(k, "id")) {
        int n = hexbytes(v, p->id, TPMS_ID_MAX);
        p->id_len = n > 0 ? (uint8_t)n : 0;
        return true;
    }
    if (!strcmp(k, "addr")) {
        uint8_t a[6];
        if (hexbytes(v, a, 6) == 6) {
            for (int i = 0; i < 6; i++) p->addr[i] = a[5 - i];   // printed most significant first
        }
        return true;
    }
    if (!strcmp(k, "low_alert")) return parse_bool(v, &c->low_on[t]);
    if (!strcmp(k, "high_alert")) return parse_bool(v, &c->high_on[t]);
    if (!strcmp(k, "low_kpa") && parse_num(v, &f) && f >= 0 && f <= 1000) return c->low_kpa[t] = f, true;
    if (!strcmp(k, "high_kpa") && parse_num(v, &f) && f >= 0 && f <= 1000) return c->high_kpa[t] = f, true;
    return false;
}

static bool apply(const char *k, const char *v, tpms_pairing_t pairs[TPMS_TYRES], tpms_cfg_t *c)
{
    for (int t = 0; t < TPMS_TYRES; t++) {
        size_t n = strlen(TYRE[t]);
        if (!strncmp(k, TYRE[t], n) && k[n] == '_') return apply_tyre(k + n + 1, v, &pairs[t], c, t);
    }
    float f;
    if (!strcmp(k, "alerts")) return parse_bool(v, &c->enabled);
    if (!strcmp(k, "hot_alert")) return parse_bool(v, &c->hot_on);
    if (!strcmp(k, "cold_alert")) return parse_bool(v, &c->cold_on);
    if (!strcmp(k, "sensor_alarm")) return parse_bool(v, &c->sensor_alarm);
    if (!strcmp(k, "battery_alert")) return parse_bool(v, &c->battery);
    if (!strcmp(k, "hot_c") && parse_num(v, &f)) return c->hot_c = f, true;
    if (!strcmp(k, "cold_c") && parse_num(v, &f)) return c->cold_c = f, true;
    if (!strcmp(k, "fast_loss_pct") && parse_num(v, &f) && f >= 0 && f <= 100) return c->loss_pct = (int)f, true;
    if (!strcmp(k, "silent_min") && parse_num(v, &f) && f >= 0 && f <= 240) return c->silent_min = (int)f, true;
    return false;
}

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) *--e = 0;
    return s;
}

bool tpms_cfgfile_parse(const char *text, tpms_pairing_t pairs[TPMS_TYRES], tpms_cfg_t *c)
{
    memset(pairs, 0, sizeof(tpms_pairing_t) * TPMS_TYRES);
    tpms_cfg_defaults(c);
    int known = 0;
    while (*text) {
        char line[128];
        size_t n = strcspn(text, "\n");
        size_t m = n < sizeof(line) - 1 ? n : sizeof(line) - 1;
        memcpy(line, text, m);
        line[m] = 0;
        text += n + (text[n] == '\n');
        char *hash = strchr(line, '#');
        if (hash) *hash = 0;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = trim(line), *v = trim(eq + 1);
        for (char *p = k; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (apply(k, v, pairs, c)) known++;
    }
    // A sensor with neither an id nor an address can't be told from another.
    for (int t = 0; t < TPMS_TYRES; t++) {
        static const uint8_t zero[6] = {0};
        if (pairs[t].id_len == 0 && memcmp(pairs[t].addr, zero, 6) == 0) pairs[t].format = TPMS_F_NONE;
        if (pairs[t].format == TPMS_F_NONE) memset(&pairs[t], 0, sizeof(pairs[t]));
    }
    return known > 0;
}
