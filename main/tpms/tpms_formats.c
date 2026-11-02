#include <math.h>
#include <string.h>
#include "tpms_formats.h"

const char *const TPMS_FORMAT_NAMES[TPMS_F_COUNT] = {"?", "Sysgration", "SYTPMS", "Michelin", "WODHMIEY", "FOBO",
                                                     "Bekubee", "Tesla", "DJTPMS"};

#define ATM_101      101.0f       // one atmosphere as the sources subtract it, kPa
#define KPA_PER_PSI  6.894757f

static int u16le(const uint8_t *b, int i) { return b[i] | (b[i + 1] << 8); }
static int u16be(const uint8_t *b, int i) { return (b[i] << 8) | b[i + 1]; }
static int32_t i32le(const uint8_t *b, int i)
{
    return (int32_t)((uint32_t)b[i] | ((uint32_t)b[i + 1] << 8) | ((uint32_t)b[i + 2] << 16)
                     | ((uint32_t)b[i + 3] << 24));
}
static int s8(int v) { return v >= 128 ? v - 256 : v; }
// Gauge pressure never goes below zero: a deflated tyre reads 0.
static float gauge(float kpa) { return kpa > 0.0f ? kpa : 0.0f; }

static void start(tpms_reading_t *r, tpms_format_t f)
{
    memset(r, 0, sizeof(*r));
    r->format = f;
    r->position = -1;
    r->kpa = r->temp_c = r->volts = NAN;
    r->battery_pct = -1;
    r->awake = true;
}

static void set_id(tpms_reading_t *r, const uint8_t *b, int n)
{
    memcpy(r->id, b, n);
    r->id_len = (uint8_t)n;
}

// 1. Sysgration / ZEEPIN / TP630 / "TPMSII". Company id 0x0100, then position
// 0x80 + n, then EA CA and the rest of the MAC. Pressure is gauge, in Pa.
static bool sysgration(const tpms_adv_t *a, tpms_reading_t *r)
{
    const uint8_t *p = a->mfr;
    int n = a->mfr_len;
    if (n < 12 || tpms_adv_company(a) != 0x0100 || p[3] != 0xEA || p[4] != 0xCA || (p[2] & 0xF0) != 0x80) {
        return false;
    }
    start(r, TPMS_F_SYSGRATION);
    r->position = p[2] & 0x0F;
    set_id(r, p + 2, 6);
    r->kpa = gauge(i32le(p, 8) / 1000.0f);
    if (n >= 16) r->temp_c = i32le(p, 12) / 100.0f;
    if (n >= 17 && p[16] <= 100) r->battery_pct = p[16];
    if (n >= 18) r->alarm = p[17] == 0x01;
    return true;
}

// 2. SYTPMS "BR" (Pecham) and "KY" (Bekubee). Service 27A5 and a 7-byte
// value: status, voltage, signed °C, absolute pressure in 0.1 psi, CRC. The
// CRC is not checked (BLE's link layer already drops damaged packets), and the
// status bits are not an alarm: a capture with the top bit set reads 41 psi.
static bool sytpms(const tpms_adv_t *a, tpms_reading_t *r)
{
    const uint8_t *p = a->mfr;
    int n = a->mfr_len;
    if (n < 5 || !tpms_adv_has_uuid(a, 0x27A5) || (n != 7 && !a->truncated)) {
        return false;
    }
    // KY sensors subtract 14.6 psi, BR ones 14.5; without a name, BR.
    int atm = strcmp(a->name, "KY") == 0 ? 146 : 145;
    start(r, TPMS_F_SYTPMS);
    r->volts = p[1] / 10.0f;
    r->temp_c = (float)s8(p[2]);
    r->kpa = gauge((u16be(p, 3) - atm) / 10.0f * KPA_PER_PSI);
    return true;
}

// 3. Michelin TMS. Company id 0x0828, product byte 01, then a frame type that
// says where (and whether) the pressure is. Absolute mbar.
static bool michelin(const tpms_adv_t *a, tpms_reading_t *r)
{
    const uint8_t *p = a->mfr;
    int n = a->mfr_len;
    if (n < 4 || tpms_adv_company(a) != 0x0828 || p[2] != 0x01) {
        return false;
    }
    int frame = p[3], at = -1;
    if (frame == 0x03 || frame == 0x04 || frame == 0x0C) {
        at = 6;
    } else if (frame == 0x05) {
        at = 14;
    } else if (frame != 0x02 && frame != 0x06) {
        return false;   // a frame type no source has decoded
    }
    start(r, TPMS_F_MICHELIN);
    if (n >= 5) r->temp_c = (float)(p[4] - 60);
    if (n >= 6) r->volts = p[5] / 100.0f + 1.0f;
    if (at >= 0 && n >= at + 2) r->kpa = gauge((u16le(p, at) - 1000) / 10.0f);
    return true;
}

// 4. WODHMIEY / ITPMS (K) / LYTPMS (Wicarlink). Company id 0x00AC. Pressure is
// a low byte at 3 and a high flag at 8, in steps of 3.144 kPa, gauge. The value
// ends with the MAC, reversed; its last three bytes are the printed id.
static bool wodhmiey(const tpms_adv_t *a, tpms_reading_t *r)
{
    const uint8_t *p = a->mfr;
    int n = a->mfr_len;
    if (n < 3 || tpms_adv_company(a) != 0x00AC) {
        return false;
    }
    start(r, TPMS_F_WODHMIEY);
    r->volts = p[2] * 0.01f + 1.22f;
    if (n >= 5) r->temp_c = (float)(p[4] - 55);
    if (n >= 6) r->alarm = p[5] != 0;
    // Other flag values belong to an older layout no capture confirms.
    if (n >= 9 && p[8] <= 1) r->kpa = (p[3] + 256 * p[8]) * 3.144f;
    if (n >= 17) set_id(r, p + 14, 3);
    return true;
}

// 5. Salutica FOBO. Company id 0x0127, or service data for UUID 012A (an
// iBeacon-style variant whose temperature offset is 40, not 50). The reading
// is the value's last 4 bytes, so a cut packet is no reading.
static bool fobo(const tpms_adv_t *a, tpms_reading_t *r)
{
    const uint8_t *q = NULL;
    int offset = 50;
    if (a->mfr_len >= 6 && tpms_adv_company(a) == 0x0127) {
        q = a->mfr + a->mfr_len - 4;
    } else if (a->service_uuid == 0x012A && a->service_len >= 4) {
        q = a->service_data + a->service_len - 4;
        offset = 40;
    }
    if (!q || a->truncated) {
        return false;
    }
    start(r, TPMS_F_FOBO);
    r->temp_c = (float)((q[1] & 0x7F) - offset);
    int word = u16be(q, 2);
    r->kpa = (float)(word & 0x3FF);
    int battery = (word >> 10) & 0x3F;
    if (battery > 0) {
        r->volts = battery / 10.0f;
    } else {
        // No battery in the word: some send it in byte 0 as two BCD digits (0x29 = 2.9 V).
        int hi = (q[0] & 0x7F) >> 4, lo = q[0] & 0x0F;
        if (hi <= 9 && lo <= 9) r->volts = hi + lo / 10.0f;
    }
    return true;
}

// 6. Bekubee "TPMS". Service A827; the company id changes with the sensor's
// state and is ignored. The service list comes last, so a receiver that cuts
// the packet at 20 bytes drops it: the name "TPMS" with an 11-byte value is
// accepted as well.
static bool bekubee(const tpms_adv_t *a, tpms_reading_t *r)
{
    const uint8_t *p = a->mfr;
    int n = a->mfr_len;
    if (n < 4) {
        return false;
    }
    bool named = strcmp(a->name, "TPMS") == 0 && n == 11;
    if (!tpms_adv_has_uuid(a, 0xA827) && !named) {
        return false;
    }
    start(r, TPMS_F_BEKUBEE);
    r->volts = p[2] * 0.01f + 1.22f;
    r->temp_c = (float)(p[3] - 50);
    if (n >= 6) r->kpa = gauge((float)(u16be(p, 4) - 100));
    if (n >= 9) set_id(r, p + 6, 3);
    return true;
}

// 7. The Tesla protocol (Tesla OEM, Autel MX-Sensor BLE). Company id 0x022B,
// which Tesla cars also advertise under, so a frame must look like a sensor's:
// a sleep frame of at most 16 bytes, or an awake one whose battery reads 1.5
// to 4.3 V. A sleep frame carries no reading.
static bool tesla(const tpms_adv_t *a, tpms_reading_t *r)
{
    const uint8_t *p = a->mfr;
    int n = a->mfr_len;
    if (n < 5 || tpms_adv_company(a) != 0x022B) {
        return false;
    }
    int mv = n >= 10 ? u16le(p, 8) : -1;
    bool plausible = mv >= 1500 && mv <= 4300;
    if (p[4] < 0x05) {
        if (n - 2 > 16) return false;
        start(r, TPMS_F_TESLA);
        r->awake = false;
        if (plausible) r->volts = mv / 1000.0f;
        return true;
    }
    if (!plausible) {
        return false;
    }
    start(r, TPMS_F_TESLA);
    r->kpa = gauge((float)(u16le(p, 5) - 100));
    r->temp_c = (float)(p[7] - 50);
    r->volts = mv / 1000.0f;
    return true;
}

// 8. DJTPMS (sold in many aftermarket motorcycle kits). Matched on the reading alone: 14 bytes
// of manufacturer data whose CRC8 checks out, with a plausible reading.
//   c0 c1 | VV TT PH PL | FF | CC | MAC x6
// c0 c1 vary (status bits, not decoded) and are covered by the CRC. VV 0.1 V,
// TT signed °C, PH PL absolute kPa big-endian, CC = CRC8 over c0..PL. With the
// name, a longer value is read by its tail. A cut packet is no reading.
#define DJ_SIZE 14

uint8_t tpms_djtpms_crc8(const uint8_t *b, int n)
{
    uint8_t crc = 0xDF;
    for (int i = 0; i < n; i++) {
        crc ^= b[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x2F) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool djtpms(const tpms_adv_t *a, tpms_reading_t *r)
{
    int n = a->mfr_len;
    if (n < 0 || a->truncated) {
        return false;
    }
    bool named = strcmp(a->name, "DJTPMS") == 0;
    if (n != DJ_SIZE && !(named && n > DJ_SIZE)) {
        return false;
    }
    const uint8_t *q = a->mfr + n - DJ_SIZE;
    if (tpms_djtpms_crc8(q, 6) != q[7]) {
        return false;
    }
    float volts = q[2] / 10.0f;
    int temp = s8(q[3]);
    int abs_kpa = u16be(q, 4);
    // A CRC alone lets one random packet in 256 through.
    if (volts < 1.8f || volts > 3.8f || temp < -40 || temp > 125 || abs_kpa > 1000) {
        return false;
    }
    start(r, TPMS_F_DJTPMS);
    r->volts = volts;
    r->temp_c = (float)temp;
    r->kpa = gauge(abs_kpa - ATM_101);
    set_id(r, q + 8, 6);
    return true;
}

bool tpms_decode(const tpms_adv_t *a, tpms_reading_t *r)
{
    if (a->mfr_len < 0 && a->service_uuid < 0) {
        return false;
    }
    return sytpms(a, r) || bekubee(a, r) || djtpms(a, r) || sysgration(a, r) || michelin(a, r) || wodhmiey(a, r)
           || fobo(a, r) || tesla(a, r);
}
