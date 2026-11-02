#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ride_summary.h"

static bool parse_json(const char *p, ride_summary_t *out);

bool ride_summary_parse(const char *text, ride_summary_t *out)
{
    memset(out, 0, sizeof(*out));
    out->batt_start = out->batt_end = -1;
    const char *q = text;
    while (*q == ' ' || *q == '\t' || *q == '\r' || *q == '\n') q++;
    return *q == '{' && parse_json(q, out);
}

// ---- ride.json: reading ------------------------------------------------------

static const char *ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

// A string at p (on its opening quote) into out; returns the character after
// it, or NULL if it never ends. \uXXXX comes out as '?'.
static const char *read_string(const char *p, char *out, size_t n)
{
    size_t len = 0;
    for (p++; *p && *p != '"'; p++) {
        char c = *p;
        if (c == '\\') {
            c = *++p;
            if (!c) return NULL;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'r') c = '\r';
            else if (c == 'b' || c == 'f') c = ' ';
            else if (c == 'u') {
                for (int i = 0; i < 4; i++) {
                    if (!*++p) return NULL;
                }
                c = '?';
            }
        }
        if (out && len + 1 < n) out[len++] = c;
    }
    if (out && n) out[len] = 0;
    return *p == '"' ? p + 1 : NULL;
}

// Past any value at p (nested objects and arrays included), or NULL.
static const char *skip_value(const char *p)
{
    p = ws(p);
    if (*p == '"') return read_string(p, NULL, 0);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = read_string(p, NULL, 0);
                if (!p) return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            if (*p == '}' || *p == ']') {
                if (--depth == 0) return p + 1;
            }
            p++;
        }
        return NULL;
    }
    const char *start = p;
    while (*p && !strchr(",}] \t\r\n", *p)) p++;
    return p > start ? p : NULL;
}

// The top-level numbers the Rides page shows, by key.
typedef struct {
    const char *key;
    size_t offset;   // of a float, or of a uint32_t when is_u32
    bool is_u32;
    uint32_t bit;
} json_field_t;

#define F(k, field, bit) {k, offsetof(ride_summary_t, field), false, bit}
#define U(k, field, bit) {k, offsetof(ride_summary_t, field), true, bit}
static const json_field_t FIELDS[] = {
    U("duration_s", duration_s, RS_DURATION),
    U("engine_s", engine_s, RS_ENGINE),
    F("distance_km", km, RS_DISTANCE),
    F("avg_kmh", avg_kmh, RS_AVG),
    F("max_kmh", max_kmh, RS_MAX_KMH),
    F("max_rpm", max_rpm, RS_MAX_RPM),
    F("max_coolant_c", max_coolant, RS_MAX_COOLANT),
    F("min_volts", min_volts, RS_MIN_VOLTS),
    F("max_lean_left_deg", lean_left, 0),
    F("max_lean_right_deg", lean_right, 0),
    F("max_g", max_g, RS_MAX_G),
    U("alerts", alerts, RS_ALERTS),
    F("fuel_raw_l", fuel_raw_l, 0),
    F("fuel_k", k, 0),
    F("fuel_raw_l100", l100_raw, RS_L100),
};
#undef F
#undef U

static bool parse_json(const char *p, ride_summary_t *out)
{
    bool lean_l = false, lean_r = false, fuel = false, k = false, format = false;
    p = ws(p + 1);
    while (*p != '}') {
        char key[32];
        if (*p != '"' || !(p = read_string(p, key, sizeof(key)))) return false;
        p = ws(p);
        if (*p != ':') return false;
        p = ws(p + 1);
        const char *end = NULL;
        bool null = strncmp(p, "null", 4) == 0;
        const json_field_t *f = NULL;
        for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]) && !null; i++) {
            if (!strcmp(key, FIELDS[i].key)) f = &FIELDS[i];
        }
        if (f) {
            char *e;
            double v = strtod(p, &e);
            if (e == p) return false;
            end = e;
            if (f->is_u32) *(uint32_t *)((char *)out + f->offset) = v > 0 ? (uint32_t)(v + 0.5) : 0;
            else *(float *)((char *)out + f->offset) = (float)v;
            out->have |= f->bit;
            lean_l |= !strcmp(key, "max_lean_left_deg");
            lean_r |= !strcmp(key, "max_lean_right_deg");
            fuel |= !strcmp(key, "fuel_raw_l");
            k |= !strcmp(key, "fuel_k");
        } else if (!strcmp(key, "format") && !null) {
            format = strtol(p, NULL, 10) >= RIDE_JSON_FORMAT;
        } else if (!strcmp(key, "in_progress")) {
            out->in_progress = strncmp(p, "true", 4) == 0;
        } else if ((!strcmp(key, "batt_start_pct") || !strcmp(key, "batt_end_pct")) && !null) {
            int v = (int)strtol(p, NULL, 10);
            if (key[5] == 's') out->batt_start = v;
            else { out->batt_end = v; out->have |= RS_BATTERY; }
        } else if (!strcmp(key, "fault_codes") && *p == '[') {
            // ["P0118","P0562"] -> "P0118 P0562"
            out->codes[0] = 0;
            p = ws(p + 1);
            while (*p == '"') {
                char code[12];
                if (!(p = read_string(p, code, sizeof(code)))) return false;
                size_t len = strlen(out->codes);
                if (len + strlen(code) + 2 <= sizeof(out->codes)) {
                    snprintf(out->codes + len, sizeof(out->codes) - len, "%s%s", len ? " " : "", code);
                }
                p = ws(p);
                if (*p == ',') p = ws(p + 1);
            }
            if (*p != ']') return false;
            end = p + 1;
            out->have |= RS_CODES;
        }
        if (!end) end = skip_value(p);
        if (!end) return false;
        p = ws(end);
        if (*p == ',') p = ws(p + 1);
        else if (*p != '}') return false;
    }
    if (lean_l && lean_r) out->have |= RS_LEAN;
    if (fuel && k) out->have |= RS_FUEL;
    return format;
}

// ---- ride.json: writing ------------------------------------------------------

void ride_report_init(ride_report_t *r)
{
    memset(r, 0, sizeof(*r));
    float *nan_fields[] = {&r->distance_km, &r->avg_kmh, &r->max_kmh, &r->max_rpm, &r->max_coolant_c,
                           &r->min_volts, &r->max_lean_left_deg, &r->max_lean_right_deg, &r->max_g,
                           &r->fuel_raw_l, &r->fuel_raw_l100, &r->fuel_k, &r->since_fill_km,
                           &r->since_fill_raw_l, &r->max_chip_c, &r->max_cell_c, &r->batt_avg_ma};
    for (size_t i = 0; i < sizeof(nan_fields) / sizeof(nan_fields[0]); i++) *nan_fields[i] = NAN;
    for (int t = 0; t < 2; t++) r->tyre[t].min_kpa = r->tyre[t].max_kpa = r->tyre[t].max_c = NAN;
    for (int i = 0; i < 3; i++) r->gyro_bias_dps[i] = NAN;
    r->fuel_model = r->fuel_coverage_pct = r->fills = -1;
    r->batt_start_pct = r->batt_end_pct = r->batt_end_mv = -1;
    r->batt_full_mah = r->batt_remaining_mah = -1;
}

typedef struct {
    char *buf;
    size_t n, len;
    bool full;
} out_t;

static void put(out_t *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(out_t *o, const char *fmt, ...)
{
    if (o->full) return;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(o->buf + o->len, o->n - o->len, fmt, ap);
    va_end(ap);
    if (k < 0 || (size_t)k >= o->n - o->len) o->full = true;
    else o->len += k;
}

static void put_str(out_t *o, const char *s)
{
    put(o, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') put(o, "\\%c", c);
        else if (c < 0x20) put(o, "\\u%04x", c);
        else put(o, "%c", c);
    }
    put(o, "\"");
}

// A number with fmt, or null when unknown (NAN, or an int below 0).
static void put_num(out_t *o, const char *fmt, float v)
{
    if (isnan(v) || isinf(v)) put(o, "null");
    else put(o, fmt, v);
}

static void put_int(out_t *o, int v)
{
    if (v < 0) put(o, "null");
    else put(o, "%d", v);
}

// A top-level line: ,\n  "key": 
static void key(out_t *o, const char *k)
{
    put(o, "%s\n  \"%s\": ", o->len > 1 ? "," : "", k);
}

size_t ride_report_json(const ride_report_t *r, char *buf, size_t n)
{
    out_t o = {buf, n, 0, false};
    put(&o, "{");
    key(&o, "format"); put(&o, "%d", RIDE_JSON_FORMAT);
    key(&o, "ride"); put(&o, "%d", r->ride);
    key(&o, "uid"); put_str(&o, r->uid);
    key(&o, "in_progress"); put(&o, r->in_progress ? "true" : "false");
    key(&o, "start");
    if (r->start[0]) put_str(&o, r->start);
    else put(&o, "null");
    key(&o, "firmware"); put_str(&o, r->firmware);
    key(&o, "bike"); put_str(&o, r->bike);
    key(&o, "duration_s"); put(&o, "%lu", (unsigned long)r->duration_s);
    key(&o, "engine_s"); put(&o, "%lu", (unsigned long)r->engine_s);
    key(&o, "distance_km"); put_num(&o, "%.2f", r->distance_km);
    key(&o, "avg_kmh"); put_num(&o, "%.0f", r->avg_kmh);
    key(&o, "max_kmh"); put_num(&o, "%.0f", r->max_kmh);
    key(&o, "max_rpm"); put_num(&o, "%.0f", r->max_rpm);
    key(&o, "max_coolant_c"); put_num(&o, "%.0f", r->max_coolant_c);
    key(&o, "min_volts"); put_num(&o, "%.2f", r->min_volts);
    key(&o, "max_lean_left_deg"); put_num(&o, "%.0f", r->max_lean_left_deg);
    key(&o, "max_lean_right_deg"); put_num(&o, "%.0f", r->max_lean_right_deg);
    key(&o, "max_g"); put_num(&o, "%.2f", r->max_g);
    key(&o, "gyro_bias_dps");
    if (isnan(r->gyro_bias_dps[0])) put(&o, "null");
    else put(&o, "[%.2f, %.2f, %.2f]", r->gyro_bias_dps[0], r->gyro_bias_dps[1], r->gyro_bias_dps[2]);
    key(&o, "alerts"); put(&o, "%lu", (unsigned long)r->alerts);
    key(&o, "fault_codes"); put(&o, "[");
    for (const char *c = r->codes; *c;) {
        size_t len = strcspn(c, " ");
        if (len) {
            char code[12];
            snprintf(code, sizeof(code), "%.*s", (int)(len < sizeof(code) ? len : sizeof(code) - 1), c);
            if (c != r->codes) put(&o, ",");
            put_str(&o, code);
        }
        c += len;
        while (*c == ' ') c++;
    }
    put(&o, "]");
    key(&o, "fuel_raw_l"); put_num(&o, "%.3f", r->fuel_raw_l);
    key(&o, "fuel_raw_l100"); put_num(&o, "%.2f", r->fuel_raw_l100);
    key(&o, "fuel_k"); put_num(&o, "%.3f", r->fuel_k);
    key(&o, "batt_start_pct"); put_int(&o, r->batt_start_pct);
    key(&o, "batt_end_pct"); put_int(&o, r->batt_end_pct);
    key(&o, "fuel"); put(&o, "{\"model\": "); put_int(&o, r->fuel_model);
    put(&o, ", \"coverage_pct\": "); put_int(&o, r->fuel_coverage_pct);
    put(&o, ", \"since_fill_km\": "); put_num(&o, "%.1f", r->since_fill_km);
    put(&o, ", \"since_fill_raw_l\": "); put_num(&o, "%.3f", r->since_fill_raw_l);
    put(&o, ", \"fills\": "); put_int(&o, r->fills); put(&o, "}");
    key(&o, "max_chip_c"); put_num(&o, "%.1f", r->max_chip_c);
    key(&o, "max_cell_c"); put_num(&o, "%.1f", r->max_cell_c);
    key(&o, "board_battery"); put(&o, "{\"cell\": %s, \"end_mv\": ", r->batt_cell ? "true" : "false");
    put_int(&o, r->batt_end_mv); put(&o, ", \"charging\": %s, \"avg_ma\": ", r->batt_charging ? "true" : "false");
    put_num(&o, "%.0f", r->batt_avg_ma);
    put(&o, ", \"full_mah\": "); put_int(&o, r->batt_full_mah);
    put(&o, ", \"remaining_mah\": "); put_int(&o, r->batt_remaining_mah); put(&o, "}");
    key(&o, "tyres"); put(&o, "{");
    static const char *const NAMES[2] = {"front", "rear"};
    for (int t = 0; t < 2; t++) {
        const ride_tyre_t *y = &r->tyre[t];
        put(&o, "%s\"%s\": {\"paired\": %s, \"min_kpa\": ", t ? ", " : "", NAMES[t], y->paired ? "true" : "false");
        put_num(&o, "%.0f", y->min_kpa); put(&o, ", \"max_kpa\": "); put_num(&o, "%.0f", y->max_kpa);
        put(&o, ", \"max_c\": "); put_num(&o, "%.0f", y->max_c);
        put(&o, ", \"heard\": "); put_int(&o, y->heard);
        put(&o, ", \"min_rssi_dbm\": ");
        if (y->min_rssi) put(&o, "%d}", y->min_rssi);
        else put(&o, "null}");
    }
    put(&o, "}");
    key(&o, "adapter"); put(&o, "{\"name\": "); put_str(&o, r->ble_name);
    put(&o, ", \"elm\": "); put_str(&o, r->adapter); put(&o, "}");
    key(&o, "ecu"); put(&o, "{\"protocol\": "); put_str(&o, r->protocol);
    put(&o, ", \"address\": "); put_str(&o, r->ecu);
    put(&o, ", \"vin\": "); put_str(&o, r->vin);
    put(&o, ", \"cal_ids\": [");
    for (int i = 0, k = 0; i < 2; i++) {
        if (!r->cal_id[i][0]) continue;
        if (k++) put(&o, ", ");
        put_str(&o, r->cal_id[i]);
    }
    put(&o, "], \"cvns\": [");
    for (int i = 0, k = 0; i < 2; i++) {
        if (!r->cvn[i][0]) continue;
        if (k++) put(&o, ", ");
        put_str(&o, r->cvn[i]);
    }
    put(&o, "]}");
    key(&o, "link");
    put(&o, "{\"timeouts\": %lu, \"reinits\": %lu, \"link_drops\": %lu, \"ecu_drops\": %lu}",
        (unsigned long)r->timeouts, (unsigned long)r->reinits, (unsigned long)r->link_drops,
        (unsigned long)r->ecu_drops);
    put(&o, "\n}\n");
    return o.full ? 0 : o.len;
}
