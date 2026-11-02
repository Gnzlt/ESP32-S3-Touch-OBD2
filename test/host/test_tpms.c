/*
 * The TPMS core (main/tpms/): every published capture of the 8 formats and
 * their worked examples, packets cut short, the tyre state,
 * the alert engine and the pairing list.
 */
#include <ctype.h>
#include <math.h>
#include "test_util.h"
#include "tpms/tpms_adv.h"
#include "tpms/tpms_alerts.h"
#include "tpms/tpms_cfgfile.h"
#include "tpms/tpms_formats.h"
#include "tpms/tpms_pair.h"
#include "tpms/tpms_tyres.h"

// "0303A527" -> 03 03 A5 27. Spaces are skipped, so captures keep their AD
// structures apart.
static int hex(const char *s, uint8_t *out)
{
    int n = 0, high = -1;
    for (; *s; s++) {
        int c = toupper((unsigned char)*s);
        int v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (v < 0) continue;
        if (high < 0) {
            high = v;
        } else {
            out[n++] = (uint8_t)(high * 16 + v);
            high = -1;
        }
    }
    return n;
}

// A manufacturer-data structure around a value, with flags before it.
static int mfr(const char *value, uint8_t *out)
{
    uint8_t v[64];
    int n = hex(value, v);
    out[0] = 0x02, out[1] = 0x01, out[2] = 0x06, out[3] = (uint8_t)(n + 1), out[4] = 0xFF;
    memcpy(out + 5, v, n);
    return n + 5;
}

static bool decode_bytes(const uint8_t *raw, int len, tpms_reading_t *r)
{
    tpms_adv_t a;
    tpms_adv_parse(&a, raw, len);
    return tpms_decode(&a, r);
}

static bool read_raw(const char *raw, tpms_reading_t *r)
{
    uint8_t b[64];
    return decode_bytes(b, hex(raw, b), r);
}

static bool read_mfr(const char *value, tpms_reading_t *r)
{
    uint8_t b[64];
    return decode_bytes(b, mfr(value, b), r);
}

static bool id_is(const tpms_reading_t *r, const char *h)
{
    uint8_t b[8];
    int n = hex(h, b);
    return r->id_len == n && memcmp(r->id, b, n) == 0;
}

#define NEAR(a, b) CHECK_NEAR((a), (b), 0.01)

void test_tpms_adv(void)
{
    uint8_t b[64];
    tpms_adv_t a;
    int n = hex("0303A527 03084252 08FF281D130105A376", b);
    tpms_adv_parse(&a, b, n);
    CHECK(tpms_adv_has_uuid(&a, 0x27A5));
    CHECK_STR(a.name, "BR");
    CHECK(a.mfr_len == 7 && a.mfr[0] == 0x28 && a.mfr[6] == 0x76);
    CHECK(tpms_adv_company(&a) == 0x1D28);
    CHECK(!a.truncated);

    n = hex("020106 0303B0FB 12FFAC00D043520008DA001D10FF1100AF2206", b);
    tpms_adv_parse(&a, b, n);
    CHECK(tpms_adv_has_uuid(&a, 0xFBB0) && !tpms_adv_has_uuid(&a, 0x27A5));
    CHECK(tpms_adv_company(&a) == 0x00AC && a.mfr_len == 17);
    CHECK(a.name[0] == 0 && !a.truncated);

    // A Sysgration packet cut to 20 bytes: the manufacturer data loses its tail.
    hex("020105 13FF000180EACA108A78E36D0000E60A00005B00", b);
    tpms_adv_parse(&a, b, 20);
    CHECK(a.truncated);
    CHECK(tpms_adv_company(&a) == 0x0100 && a.mfr_len == 15);

    n = hex("020106 09", b);   // a length byte with nothing after it
    tpms_adv_parse(&a, b, n);
    CHECK(a.truncated && a.mfr_len < 0);

    n = hex("02010605FF1234560000000000", b);   // zero padding
    tpms_adv_parse(&a, b, n);
    CHECK(a.mfr_len == 4 && a.mfr[3] == 0x00 && !a.truncated);

    n = hex("020106 07162A0100467CE6", b);
    tpms_adv_parse(&a, b, n);
    CHECK(a.service_uuid == 0x012A && a.service_len == 4 && a.service_data[3] == 0xE6);
    CHECK(tpms_adv_company(&a) == -1);

    n = hex("050954C3A953", b);
    tpms_adv_parse(&a, b, n);
    CHECK_STR(a.name, "T??S");

    tpms_adv_parse(&a, b, 0);
    CHECK(a.mfr_len < 0);
    b[0] = 0;
    tpms_adv_parse(&a, b, 1);
    CHECK(a.mfr_len < 0 && !a.truncated);
}

static void sysgration(void)
{
    tpms_reading_t r;
    CHECK(read_mfr("0001 80EACA108A78 E36D0000 E60A0000 5B 00", &r));
    CHECK(r.format == TPMS_F_SYSGRATION && r.position == 0);
    CHECK(id_is(&r, "80EACA108A78"));
    NEAR(r.kpa, 28.131);
    NEAR(r.temp_c, 27.90);
    CHECK(r.battery_pct == 91 && !r.alarm);

    CHECK(read_mfr("0001 80EACA502ED8 58090300 1C070000 5F 00", &r));
    NEAR(r.kpa, 199.0);
    NEAR(r.temp_c, 18.20);
    CHECK(r.battery_pct == 95);

    // Rear right, -5.00 °C, the alarm set, battery byte 0xC8 (not a percentage).
    CHECK(read_mfr("0001 83EACA000001 00000000 0CFEFFFF C8 01", &r));
    CHECK(r.position == 3);
    NEAR(r.kpa, 0.0);
    NEAR(r.temp_c, -5.0);
    CHECK(r.battery_pct == -1 && r.alarm);

    // Cut to 20 bytes: the pressure survives, the temperature doesn't.
    uint8_t b[64];
    hex("020105 13FF 0001 80EACA108A78 E36D0000 E60A0000 5B 00", b);
    CHECK(decode_bytes(b, 20, &r));
    NEAR(r.kpa, 28.131);
    CHECK(isnan(r.temp_c) && r.battery_pct == -1);
    CHECK(!decode_bytes(b, 16, &r));

    CHECK(!read_mfr("0001 80EACB108A78 E36D0000 E60A0000 5B 00", &r));
    CHECK(!read_mfr("0001 70EACA108A78 E36D0000 E60A0000 5B 00", &r));
    CHECK(!read_mfr("0002 80EACA108A78 E36D0000 E60A0000 5B 00", &r));
}

static void sytpms(void)
{
    tpms_reading_t r;
    CHECK(read_raw("0303A527 03084252 08FF 28 1D 13 0105 A376", &r));
    CHECK(r.format == TPMS_F_SYTPMS);
    NEAR(r.volts, 2.9);
    NEAR(r.temp_c, 19.0);
    NEAR(r.kpa, 79.98);
    CHECK(!r.alarm && r.id_len == 0);

    static const struct { const char *raw; float kpa, c; } cases[] = {
        {"0303A5270308425208FF101B0F026AB017", 326.12f, 15.0f},
        {"0303A5270308425208FF801C12022B5593", 282.69f, 18.0f},
        {"0303A5270308425208FF101D10022F4C32", 285.44f, 16.0f},
        {"0303A5270308425208FF801B10020F940F", 263.38f, 16.0f},
        {"0303A5270308425208FF801B1301F48D18", 244.76f, 19.0f},
        {"0303A5270308425208FF401C1400921A7F", 0.69f, 20.0f},
        {"0303A5270308425208FF241D1401D4BFEC", 222.70f, 20.0f},
        {"0303a5270308425208ff28201a01032fec", 78.60f, 26.0f},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        CHECK(read_raw(cases[i].raw, &r));
        NEAR(r.kpa, cases[i].kpa);
        NEAR(r.temp_c, cases[i].c);
    }

    // KY subtracts 14.6 psi; without a name, BR's 14.5.
    CHECK(read_raw("0303A527 0308 4B59 08FF 28 1D 13 0105 A376", &r));
    NEAR(r.kpa, 79.29);
    CHECK(read_raw("0303A527 08FF 28 1D 13 0105 A376", &r));
    NEAR(r.kpa, 79.98);

    CHECK(read_raw("0303A527 08FF 00 1E FD 008C 0000", &r));
    NEAR(r.temp_c, -3.0);
    NEAR(r.kpa, 0.0);

    CHECK(!read_raw("03084252 08FF 28 1D 13 0105 A376", &r));       // no 27A5
    CHECK(!read_raw("0303A527 09FF 28 1D 13 0105 A376 00", &r));    // 8 bytes
    uint8_t b[64];
    hex("0303A527 03084252 08FF 28 1D 13 0105 A376", b);
    CHECK(decode_bytes(b, 15, &r));                                  // cut after the pressure
    NEAR(r.kpa, 79.98);
}

static void michelin(void)
{
    tpms_reading_t r;
    CHECK(read_mfr("2808 01 03 50 C8 C40D 0000000000000000", &r));
    CHECK(r.format == TPMS_F_MICHELIN);
    NEAR(r.temp_c, 20.0);
    NEAR(r.volts, 3.0);
    NEAR(r.kpa, 252.4);
    CHECK(read_mfr("2808 01 05 3C 64 FFFF 000000000000 D007", &r));
    NEAR(r.kpa, 100.0);
    NEAR(r.temp_c, 0.0);
    NEAR(r.volts, 2.0);
    CHECK(read_mfr("2808 01 02 46 B4 0000000000000000", &r));
    CHECK(isnan(r.kpa));
    NEAR(r.temp_c, 10.0);

    CHECK(!read_mfr("2808 02 03 50 C8 C40D 0000000000000000", &r));
    CHECK(!read_mfr("2808 01 07 50 C8 C40D 0000000000000000", &r));
    CHECK(read_mfr("2808 01 05 3C 64 FFFF 00", &r));
    CHECK(isnan(r.kpa));
}

static void wodhmiey(void)
{
    tpms_reading_t r;
    const char *capture = "020106 0303B0FB 12FF AC00 D0 43 52 00 08 DA 00 1D 10 FF 11 00 AF 22 06";
    CHECK(read_raw(capture, &r));
    CHECK(r.format == TPMS_F_WODHMIEY);
    NEAR(r.volts, 3.30);
    NEAR(r.kpa, 210.65);
    NEAR(r.temp_c, 27.0);
    CHECK(!r.alarm && id_is(&r, "AF2206"));

    CHECK(read_mfr("AC00 D0 10 37 02 00 00 01", &r));
    NEAR(r.kpa, 855.17);
    NEAR(r.temp_c, 0.0);
    CHECK(r.alarm && r.id_len == 0);

    CHECK(read_mfr("AC00 D0 10 37 00 00 00 05 40", &r));
    CHECK(isnan(r.kpa));
    uint8_t b[64];
    hex(capture, b);
    CHECK(decode_bytes(b, 17, &r));
    CHECK(isnan(r.kpa));
    NEAR(r.temp_c, 27.0);
}

static void fobo(void)
{
    tpms_reading_t r;
    CHECK(read_mfr("2701 00 46 7CE6", &r));
    CHECK(r.format == TPMS_F_FOBO);
    NEAR(r.temp_c, 20.0);
    NEAR(r.kpa, 230.0);
    NEAR(r.volts, 3.1);
    CHECK(read_mfr("2701 AABBCC 00 C6 7CE6", &r));
    NEAR(r.temp_c, 20.0);
    NEAR(r.kpa, 230.0);

    CHECK(read_raw("020106 07162A01 29 46 00E6", &r));
    NEAR(r.temp_c, 30.0);
    NEAR(r.kpa, 230.0);
    NEAR(r.volts, 2.9);

    uint8_t b[64];
    int n = mfr("2701 AABBCC 00 C6 7CE6", b);
    CHECK(!decode_bytes(b, n - 1, &r));
}

static void bekubee(void)
{
    tpms_reading_t r;
    const char *capture = "050854504D53 0CFF 0200 AF4500A7002D56A8B4 030327A8";
    CHECK(read_raw(capture, &r));
    CHECK(r.format == TPMS_F_BEKUBEE);
    NEAR(r.volts, 2.97);
    NEAR(r.temp_c, 19.0);
    NEAR(r.kpa, 67.0);
    CHECK(id_is(&r, "002D56"));
    CHECK(read_raw("050854504D53 0CFF 0600 B34A0064002FA0057D 030327A8", &r));
    NEAR(r.kpa, 0.0);
    NEAR(r.temp_c, 24.0);

    uint8_t b[64];
    hex(capture, b);
    CHECK(decode_bytes(b, 20, &r));   // the service list cut off: found by its name
    NEAR(r.kpa, 67.0);
    CHECK(!read_mfr("0200 AF4500A7002D56A8B4", &r));
}

static void tesla(void)
{
    tpms_reading_t r;
    CHECK(read_mfr("2B02 01 02 0A 8601 48 EA0B", &r));
    CHECK(r.format == TPMS_F_TESLA && r.awake);
    NEAR(r.kpa, 290.0);
    NEAR(r.temp_c, 22.0);
    NEAR(r.volts, 3.05);

    CHECK(read_raw("020106 06FF2B0201FE03", &r));   // the real Autel sleep frame
    CHECK(!r.awake && isnan(r.kpa) && isnan(r.temp_c) && isnan(r.volts));
    CHECK(read_mfr("2B02 01 02 03 8601 48 B80B", &r));
    CHECK(!r.awake && isnan(r.kpa));
    NEAR(r.volts, 3.0);

    CHECK(!read_mfr("2B02 01 02 0A 8601 48 0001", &r));
    CHECK(!read_mfr("2B02 0102030405060708090A0B0C0D0E0F1011", &r));
    CHECK(!read_mfr("2B02 01 02 0A 8601", &r));
}

static void djtpms(void)
{
    tpms_reading_t r;
    CHECK(read_mfr("0000 1F1D 0144 00 CB 0C3D5E4E8BE6", &r));
    CHECK(r.format == TPMS_F_DJTPMS);
    NEAR(r.volts, 3.1);
    NEAR(r.temp_c, 29.0);
    NEAR(r.kpa, 223.0);
    CHECK(id_is(&r, "0C3D5E4E8BE6"));
    CHECK(read_mfr("0000 1E1E 012A 00 4C 0C3D5E4E8B8A", &r));
    NEAR(r.volts, 3.0);
    NEAR(r.temp_c, 30.0);
    NEAR(r.kpa, 197.0);
    CHECK(id_is(&r, "0C3D5E4E8B8A"));
    CHECK(read_mfr("0000 1F27 0154 00 06 0C3D5E4E8BE6", &r));
    NEAR(r.temp_c, 39.0);
    NEAR(r.kpa, 239.0);

    // A kit sensor as a phone received it, MAC replaced: FFE0, the name, the value.
    CHECK(read_raw("0302E0FF 0708444A54504D53 0FFF 0004 2017 0156 00 66 112233445566", &r));
    CHECK(r.format == TPMS_F_DJTPMS);
    NEAR(r.volts, 3.2);
    NEAR(r.temp_c, 23.0);
    NEAR(r.kpa, 241.0);
    CHECK(id_is(&r, "112233445566"));
    CHECK(read_raw("0302E0FF 0FFF 0004 2017 0156 00 66 112233445566", &r));

    // Named, a longer value is read by its tail; without the name it is some other device.
    CHECK(read_raw("0709 444A54504D53 11FF AABB 0000 1F1D 0144 00 CB 0C3D5E4E8BE6", &r));
    NEAR(r.kpa, 223.0);
    CHECK(!read_mfr("AABB 0000 1F1D 0144 00 CB 0C3D5E4E8BE6", &r));

    CHECK(!read_mfr("0000 1F1D 0145 00 CB 0C3D5E4E8BE6", &r));   // a pressure bit flipped
    uint8_t q[6];
    hex("0000 051D 0144", q);
    char packet[64];
    snprintf(packet, sizeof(packet), "0000 051D 0144 00 %02X 0C3D5E4E8BE6", tpms_djtpms_crc8(q, 6));
    CHECK(!read_mfr(packet, &r));                                 // a valid CRC over 0.5 V

    hex("00001F1D0144", q);
    CHECK(tpms_djtpms_crc8(q, 6) == 0xCB);
    hex("00001E1E012A", q);
    CHECK(tpms_djtpms_crc8(q, 6) == 0x4C);
    hex("00001F270154", q);
    CHECK(tpms_djtpms_crc8(q, 6) == 0x06);
}

void test_tpms_formats(void)
{
    sysgration();
    sytpms();
    michelin();
    wodhmiey();
    fobo();
    bekubee();
    tesla();
    djtpms();

    tpms_reading_t r;
    CHECK(!read_mfr("4C00 0215 FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFE0 0001 0002 C5", &r));   // an iBeacon
    CHECK(!read_mfr("0600 0109 2002 1A2B3C4D5E", &r));                                  // a Microsoft beacon
    CHECK(!read_raw("", &r));
    CHECK(!read_raw("020106", &r));
    CHECK_STR(TPMS_FORMAT_NAMES[TPMS_F_DJTPMS], "DJTPMS");
}

static tpms_reading_t reading(float kpa, float c)
{
    tpms_reading_t r = {.format = TPMS_F_SYTPMS, .position = -1, .kpa = kpa, .temp_c = c, .volts = NAN,
                        .battery_pct = -1, .awake = true};
    return r;
}

static void put(tpms_tyre_t *t, float kpa, float c, int64_t now)
{
    tpms_reading_t r = reading(kpa, c);
    tpms_tyre_update(t, &r, now);
}

void test_tpms_tyres(void)
{
    tpms_tyre_t t;
    tpms_tyre_init(&t);
    CHECK(tpms_tyre_age(&t, 1000) == -1);
    put(&t, 220, 18, 1000);
    put(&t, 221, NAN, 2000);   // cut short: no temperature
    NEAR(t.kpa, 221.0);
    NEAR(t.temp_c, 18.0);
    put(&t, NAN, 19, 3000);    // a frame without pressure
    NEAR(t.kpa, 221.0);
    CHECK(t.value_at == 2000 && t.heard_at == 3000 && tpms_tyre_age(&t, 5000) == 3000);

    // A sleep frame only proves the sensor alive.
    tpms_tyre_init(&t);
    put(&t, 250, 20, 1000);
    tpms_reading_t sleep = reading(NAN, NAN);
    sleep.awake = false;
    sleep.volts = 3.0f;
    tpms_tyre_update(&t, &sleep, 60000);
    NEAR(t.kpa, 250.0);
    NEAR(t.volts, 3.0);
    CHECK(t.heard_at == 60000 && tpms_tyre_age(&t, 60000) == 59000);

    // 250 kPa, then losing 10 kPa a minute.
    tpms_tyre_init(&t);
    for (int m = 0; m <= 6; m++) put(&t, 250.0f - 10.0f * m, 20, m * 60000);
    NEAR(tpms_tyre_drop(&t, 360000, 300000), 50.0);
    NEAR(tpms_tyre_drop(&t, 360000, 60000), 10.0);
    tpms_tyre_init(&t);
    put(&t, 230, 20, 0);
    put(&t, 240, 20, 60000);
    NEAR(tpms_tyre_drop(&t, 60000, 300000), 0.0);   // a rise is no drop

    // A burst within one step keeps one sample, the step's lowest; the ring wraps.
    tpms_tyre_init(&t);
    put(&t, 250, 20, 0);
    put(&t, 245, 20, 1000);
    put(&t, 248, 20, 2000);
    NEAR(tpms_tyre_drop(&t, 2000, 300000), 0.0);
    for (int s = 1; s <= 60; s++) put(&t, 200, 20, s * 30000);
    NEAR(tpms_tyre_drop(&t, 60 * 30000, 300000), 0.0);
    tpms_tyre_init(&t);
    CHECK(isnan(t.kpa) && tpms_tyre_age(&t, 0) == -1);

    // The battery.
    CHECK(tpms_battery_level(-1, NAN) == -1 && !tpms_battery_low(-1, NAN));
    CHECK(tpms_battery_level(91, NAN) == 91);
    CHECK(tpms_battery_level(0, 2.9f) == 0);
    CHECK(tpms_battery_level(150, 2.75f) == 50);
    CHECK(tpms_battery_level(150, NAN) == -1);
    CHECK(tpms_battery_level(-1, 3.0f) == 100 && tpms_battery_level(-1, 3.3f) == 100);
    CHECK(tpms_battery_level(-1, 2.5f) == 0 && tpms_battery_level(-1, 1.8f) == 0);
    CHECK(tpms_battery_low(10, NAN) && !tpms_battery_low(11, NAN));
    CHECK(tpms_battery_low(-1, 2.49f) && !tpms_battery_low(-1, 2.5f));

    // Units.
    NEAR(tpms_to_kpa(2.2f, TPMS_UNIT_BAR), 220.0);
    NEAR(tpms_to_kpa(32.0f, TPMS_UNIT_PSI), 220.63);
    NEAR(tpms_from_kpa(250.0f, TPMS_UNIT_PSI), 36.26);
    char s[16];
    tpms_fmt_pressure(s, sizeof(s), 225.0f, TPMS_UNIT_BAR);
    CHECK_STR(s, "2.25");
    tpms_fmt_pressure(s, sizeof(s), 225.0f, TPMS_UNIT_PSI);
    CHECK_STR(s, "32.6");
    tpms_fmt_pressure(s, sizeof(s), 225.4f, TPMS_UNIT_KPA);
    CHECK_STR(s, "225");
}

// Front 200-260 kPa, rear 220-280 kPa, temperature 0-80 °C, a 15 % loss,
// silent after 10 minutes.
static tpms_cfg_t cfg(void)
{
    tpms_cfg_t c;
    tpms_cfg_defaults(&c);
    CHECK(c.silent_min == 30);
    c.low_on[0] = c.low_on[1] = c.high_on[0] = c.high_on[1] = true;
    c.low_kpa[0] = 200, c.low_kpa[1] = 220;
    c.high_kpa[0] = 260, c.high_kpa[1] = 280;
    c.cold_on = true, c.cold_c = 0;
    c.hot_on = true, c.hot_c = 80;
    c.loss_pct = 15;
    c.silent_min = 10;
    return c;
}

static void both(tpms_tyre_t t[2], float front, float rear, float c, int64_t now)
{
    tpms_tyre_init(&t[0]);
    tpms_tyre_init(&t[1]);
    put(&t[0], front, c, now);
    put(&t[1], rear, c, now);
}

static const bool BOTH[2] = {true, true};

void test_tpms_alerts(void)
{
    tpms_cfg_t c = cfg();
    tpms_alerts_t al;
    tpms_tyre_t t[2];

    // Once per crossing, with hysteresis.
    tpms_alerts_init(&al);
    both(t, 230, 250, 20, 0);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true) == TPMS_A_NONE);
    put(&t[0], 199, 20, 1000);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 1000, true) == TPMS_A_NONE);   // it must hold first
    put(&t[0], 199, 20, 21000);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 21000, true) == TPMS_A_LOW && al.slot == TPMS_FRONT);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 22000, true) == TPMS_A_NONE);
    put(&t[0], 202, 20, 23000);   // inside the hysteresis
    tpms_alerts_evaluate(&al, &c, t, BOTH, 23000, true);
    put(&t[0], 199, 20, 24000);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 24000, true) == TPMS_A_NONE);
    put(&t[0], 206, 20, 25000);   // clear: re-arms
    tpms_alerts_evaluate(&al, &c, t, BOTH, 25000, true);
    put(&t[0], 199, 20, 26000);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 26000, true) == TPMS_A_NONE);
    put(&t[0], 199, 20, 46000);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 46000, true) == TPMS_A_LOW);

    // On a test ride each DJTPMS sensor's first packets after waking read 1-2 kPa for
    // a few seconds, then the real pressure. No alert, low or loss.
    tpms_alerts_init(&al);
    both(t, 230, 250, 20, 0);
    tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true);
    bool fired = false;
    for (int64_t ms = 1000; ms <= 120000; ms += 1000) {
        if (ms == 55000) put(&t[1], 1, 18, ms);
        if (ms == 59000) put(&t[1], 250, 18, ms);
        if (ms == 86000 || ms == 90000) put(&t[0], 2, 17, ms);
        if (ms == 95000) put(&t[0], 228, 17, ms);
        fired |= tpms_alerts_evaluate(&al, &c, t, BOTH, ms, true) != TPMS_A_NONE;
    }
    CHECK(!fired);
    // A lone low packet, then nothing: no later packet confirms it.
    tpms_alerts_init(&al);
    both(t, 230, 250, 20, 0);
    put(&t[0], 2, 17, 1000);
    fired = false;
    for (int64_t ms = 1000; ms <= 120000; ms += 1000) fired |= tpms_alerts_evaluate(&al, &c, t, BOTH, ms, true) != TPMS_A_NONE;
    CHECK(!fired);

    // The low alert switched off keeps quiet; its number stays for later.
    tpms_cfg_t off_low = c;
    off_low.low_on[TPMS_FRONT] = false;
    tpms_alerts_init(&al);
    both(t, 150, 250, 20, 0);
    CHECK(tpms_alerts_evaluate(&al, &off_low, t, BOTH, 0, true) == TPMS_A_NONE);
    CHECK(off_low.low_kpa[TPMS_FRONT] == 200);

    // Each tyre's own limits.
    tpms_alerts_init(&al);
    both(t, 215, 215, 20, 0);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true) == TPMS_A_NONE);
    both(t, 215, 215, 20, 20000);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 20000, true) == TPMS_A_LOW && al.slot == TPMS_REAR);
    tpms_alerts_init(&al);
    both(t, 270, 270, 20, 0);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true) == TPMS_A_HIGH && al.slot == TPMS_FRONT);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true) == TPMS_A_NONE);

    // Temperature both ways.
    tpms_alerts_init(&al);
    both(t, 230, 250, 85, 0);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true) == TPMS_A_HOT);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true) == TPMS_A_HOT);   // the other tyre
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true) == TPMS_A_NONE);
    tpms_alerts_init(&al);
    both(t, 230, 250, -2, 0);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true) == TPMS_A_COLD);
    tpms_cfg_t no_cold = c;
    no_cold.cold_on = false;
    tpms_alerts_init(&al);
    CHECK(tpms_alerts_evaluate(&al, &no_cold, t, BOTH, 0, true) == TPMS_A_NONE);

    // Fast loss comes before low.
    tpms_alerts_init(&al);
    both(t, 240, 250, 20, 0);
    tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true);
    put(&t[0], 190, 20, 120000);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 120000, true) == TPMS_A_NONE);
    put(&t[0], 190, 20, 140000);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 140000, true) == TPMS_A_LOSS);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 140000, true) == TPMS_A_LOW);

    // Fast loss is a share of the pressure, and needs 10 kPa.
    tpms_alerts_init(&al);
    both(t, 240, 250, 20, 0);
    tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true);
    put(&t[1], 225, 20, 60000);   // 10 %
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 60000, true) == TPMS_A_NONE);
    put(&t[1], 210, 20, 120000);  // 16 %
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 120000, true) == TPMS_A_NONE);
    put(&t[1], 210, 20, 140000);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 140000, true) == TPMS_A_LOSS && al.slot == TPMS_REAR);
    tpms_cfg_t no_low = c;
    no_low.low_on[0] = no_low.low_on[1] = false;
    tpms_alerts_init(&al);
    both(t, 20, 20, 20, 0);
    put(&t[0], 12, 20, 60000);    // a sensor off its tyre: 40 %, but only 8 kPa
    CHECK(tpms_alerts_evaluate(&al, &no_low, t, BOTH, 60000, true) == TPMS_A_NONE);

    // Silence only while riding.
    tpms_alerts_init(&al);
    both(t, 230, 250, 20, 0);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 700000, false) == TPMS_A_NONE);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 700000, true) == TPMS_A_SILENT);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 700000, true) == TPMS_A_SILENT);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 800000, true) == TPMS_A_NONE);
    put(&t[0], 230, 20, 900000);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 1600000, true) == TPMS_A_SILENT && al.slot == TPMS_FRONT);

    // The sensor's alarm and battery; unpaired tyres are ignored.
    tpms_alerts_init(&al);
    both(t, 230, 250, 20, 0);
    tpms_reading_t r = reading(230, NAN);
    r.format = TPMS_F_SYSGRATION;
    r.alarm = true;
    r.battery_pct = 5;
    tpms_tyre_update(&t[1], &r, 0);
    const bool front_only[2] = {true, false};
    CHECK(tpms_alerts_evaluate(&al, &c, t, front_only, 0, true) == TPMS_A_NONE);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true) == TPMS_A_ALARM);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true) == TPMS_A_BATTERY);
    CHECK(tpms_alerts_evaluate(&al, &c, t, BOTH, 0, true) == TPMS_A_NONE);

    // Switched off, or nothing set.
    tpms_cfg_t disabled = c;
    disabled.enabled = false;
    tpms_alerts_init(&al);
    both(t, 100, 100, 100, 0);
    CHECK(tpms_alerts_evaluate(&al, &disabled, t, BOTH, 0, true) == TPMS_A_NONE);
    tpms_cfg_t quiet;
    tpms_cfg_defaults(&quiet);
    quiet.low_on[0] = quiet.low_on[1] = false;
    quiet.hot_on = false;
    quiet.silent_min = 0;
    tpms_alerts_init(&al);
    CHECK(tpms_alerts_evaluate(&al, &quiet, t, BOTH, 0, true) == TPMS_A_NONE);

    // The defaults: low alerts on at the bike's numbers.
    tpms_cfg_defaults(&quiet);
    CHECK(quiet.low_on[0] && quiet.low_on[1] && !quiet.high_on[0]);
    NEAR(quiet.low_kpa[0], 180.0);
    NEAR(quiet.low_kpa[1], 200.0);
}

void test_tpms_pair(void)
{
    static const uint8_t A1[6] = {1, 2, 3, 4, 5, 6}, A2[6] = {9, 9, 9, 9, 9, 9}, A3[6] = {7, 7, 7, 7, 7, 7};
    tpms_reading_t dj, br;
    CHECK(read_mfr("0000 1F1D 0144 00 CB 0C3D5E4E8BE6", &dj));
    CHECK(read_raw("0303A527 08FF 28 1D 13 0105 A376", &br));

    // By payload id when the format has one (the address may differ), else by address.
    tpms_pairing_t p;
    tpms_pairing_set(&p, &dj, A1);
    CHECK(tpms_pairing_matches(&p, &dj, A2));
    tpms_reading_t other = dj;
    other.id[5] = 0x8A;
    CHECK(!tpms_pairing_matches(&p, &other, A1));
    tpms_pairing_set(&p, &br, A1);
    CHECK(tpms_pairing_matches(&p, &br, A1) && !tpms_pairing_matches(&p, &br, A2));
    CHECK(!tpms_pairing_matches(&p, &dj, A1));   // another format
    tpms_pairing_t none = {0};
    CHECK(!tpms_pairing_matches(&none, &br, A1));

    tpms_pairing_t q;
    tpms_pairing_set(&p, &dj, A1);
    tpms_pairing_set(&q, &dj, A2);
    CHECK(tpms_pairing_same(&p, &q));             // the same id from another address
    tpms_pairing_set(&q, &other, A1);
    CHECK(!tpms_pairing_same(&p, &q));
    CHECK(!tpms_pairing_same(&none, &none));

    char tail[7];
    tpms_pairing_set(&p, &dj, A1);
    tpms_pairing_tail(&p, tail);
    CHECK_STR(tail, "4E8BE6");
    tpms_pairing_set(&p, &br, A1);
    tpms_pairing_tail(&p, tail);
    CHECK_STR(tail, "030201");

    // Strongest first, one entry per sensor, stale ones left out.
    tpms_cands_t l;
    tpms_cands_clear(&l);
    tpms_cands_offer(&l, &br, A1, -80, 0);
    tpms_cands_offer(&l, &dj, A2, -60, 1000);
    tpms_cands_offer(&l, &br, A3, -70, 2000);
    tpms_cands_offer(&l, &br, A1, -50, 3000);   // the same sensor, closer
    CHECK(l.n == 3);
    int idx[TPMS_CAND_MAX];
    int n = tpms_cands_fresh(&l, 3000, idx);
    CHECK(n == 3 && l.c[idx[0]].rssi == -50 && l.c[idx[1]].rssi == -60 && l.c[idx[2]].rssi == -70);
    n = tpms_cands_fresh(&l, 21500, idx);   // 20 s on: the one heard at 1000 has gone
    CHECK(n == 2 && l.c[idx[0]].rssi == -50);

    // Full: a stronger newcomer replaces the weakest, a weaker one is dropped.
    tpms_cands_clear(&l);
    for (int i = 0; i < TPMS_CAND_MAX; i++) {
        uint8_t a[6] = {(uint8_t)i, 0, 0, 0, 0, 0};
        tpms_cands_offer(&l, &br, a, -60 - i, 0);
    }
    uint8_t weak[6] = {0xEE}, strong[6] = {0xFF};
    tpms_cands_offer(&l, &br, weak, -90, 0);
    CHECK(l.n == TPMS_CAND_MAX && l.c[TPMS_CAND_MAX - 1].rssi == -67);
    tpms_cands_offer(&l, &br, strong, -40, 0);
    CHECK(l.c[TPMS_CAND_MAX - 1].rssi == -40);
}

void test_tpms_cfgfile(void)
{
    tpms_reading_t dj, br;
    CHECK(read_mfr("0000 1F1D 0144 00 CB 0C3D5E4E8BE6", &dj));
    CHECK(read_raw("0303A527 08FF 28 1D 13 0105 A376", &br));
    static const uint8_t A1[6] = {0x26, 0x55, 0x44, 0x72, 0xAB, 0x3C}, A2[6] = {1, 2, 3, 4, 5, 6};
    tpms_pairing_t pairs[2], back[2];
    tpms_pairing_set(&pairs[0], &dj, A1);
    tpms_pairing_set(&pairs[1], &br, A2);   // no payload id: known by its address
    tpms_cfg_t c, c2;
    tpms_cfg_defaults(&c);
    c.low_kpa[0] = 175, c.low_on[1] = false, c.high_on[1] = true, c.high_kpa[1] = 290;
    c.hot_c = 70, c.silent_min = 0, c.battery = false;

    // Round trip.
    char text[2048];
    int n = tpms_cfgfile_write(text, sizeof(text), pairs, &c);
    CHECK(n > 0 && n < (int)sizeof(text) - 1);
    CHECK(strstr(text, "front_format = DJTPMS\n") && strstr(text, "front_id = 0C3D5E4E8BE6\n"));
    CHECK(strstr(text, "front_addr = 3C:AB:72:44:55:26\n") && strstr(text, "rear_id = \n"));
    CHECK(tpms_cfgfile_parse(text, back, &c2));
    CHECK(memcmp(back, pairs, sizeof(pairs)) == 0);
    CHECK(c2.low_kpa[0] == 175 && !c2.low_on[1] && c2.high_on[1] && c2.high_kpa[1] == 290);
    CHECK(c2.hot_c == 70 && c2.silent_min == 0 && !c2.battery && c2.enabled && c2.loss_pct == 15);
    tpms_reading_t r = dj;
    CHECK(tpms_pairing_matches(&back[0], &r, A2));   // by id, whatever the address
    CHECK(tpms_pairing_matches(&back[1], &br, A2));

    // Hand-written: comments, spacing, case, unknown keys; missing keys keep their defaults.
    CHECK(tpms_cfgfile_parse("# mine\r\n  REAR_FORMAT = djtpms \r\nrear_id=0C3D5E4E8B8A  # the rear\n"
                             "rear_low_alert = OFF\nwheels = 2\n", back, &c2));
    CHECK(back[0].format == TPMS_F_NONE && back[1].format == TPMS_F_DJTPMS && back[1].id_len == 6);
    CHECK(!c2.low_on[1] && c2.low_on[0] && c2.low_kpa[0] == 180);

    // A sensor with no id and no address can't be paired; bad numbers keep the default.
    CHECK(tpms_cfgfile_parse("front_format = SYTPMS\nfront_low_kpa = lots\n", back, &c2));
    CHECK(back[0].format == TPMS_F_NONE && c2.low_kpa[0] == 180);

    // Not this file at all: nothing to apply.
    CHECK(!tpms_cfgfile_parse("", back, &c2));
    CHECK(!tpms_cfgfile_parse("t_s,rpm,kmh\n1,2,3\n", back, &c2));

    // Too small a buffer is cut, never overrun.
    char small[40];
    n = tpms_cfgfile_write(small, sizeof(small), pairs, &c);
    CHECK(n == (int)sizeof(small) - 1 && strlen(small) == sizeof(small) - 1);
}
