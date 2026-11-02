#include "rtc_regs.h"

static uint8_t to_bcd(int v) { return (uint8_t)((v / 10) << 4 | v % 10); }

static int from_bcd(uint8_t b, uint8_t mask)
{
    b &= mask;
    return (b >> 4) > 9 || (b & 0x0F) > 9 ? -1 : (b >> 4) * 10 + (b & 0x0F);
}

// Days since 1970-01-01 for a civil date, and back (H. Hinnant's algorithms).
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int yoe = (int)(y - era * 400);
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int doe = (int)(z - era * 146097);
    int yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = (int)(yoe + era * 400) + (*m <= 2);
}

void rtc_regs_encode(int64_t epoch, uint8_t regs[RTC_REGS_N])
{
    int64_t days = epoch / 86400, secs = epoch % 86400;
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    regs[0] = to_bcd((int)(secs % 60));
    regs[1] = to_bcd((int)(secs / 60 % 60));
    regs[2] = to_bcd((int)(secs / 3600));
    regs[3] = to_bcd(d);
    regs[4] = (uint8_t)((days + 4) % 7);   // 1970-01-01 was a Thursday; 0 = Sunday
    regs[5] = to_bcd(m);
    regs[6] = to_bcd(y - 2000);
}

bool rtc_regs_decode(const uint8_t regs[RTC_REGS_N], int64_t *epoch)
{
    if (regs[0] & 0x80) return false;
    int s = from_bcd(regs[0], 0x7F), mi = from_bcd(regs[1], 0x7F), h = from_bcd(regs[2], 0x3F);
    int d = from_bcd(regs[3], 0x3F), m = from_bcd(regs[5], 0x1F), y = from_bcd(regs[6], 0xFF);
    if (s < 0 || s > 59 || mi < 0 || mi > 59 || h < 0 || h > 23 || d < 1 || d > 31 || m < 1 || m > 12 || y < 0) {
        return false;
    }
    *epoch = days_from_civil(2000 + y, m, d) * 86400 + h * 3600 + mi * 60 + s;
    return true;
}
