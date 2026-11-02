#include <string.h>
#include "tpms_adv.h"

// AD types (Bluetooth Assigned Numbers, "Common Data Types").
#define UUIDS16_PART   0x02
#define UUIDS16_ALL    0x03
#define NAME_SHORT     0x08
#define NAME_FULL      0x09
#define SERVICE_DATA16 0x16
#define MANUFACTURER   0xFF

static void add(tpms_adv_t *a, uint8_t type, const uint8_t *v, int n)
{
    if (type == MANUFACTURER) {
        if (a->mfr_len < 0) {
            a->mfr = v;
            a->mfr_len = n;
        }
    } else if (type == UUIDS16_PART || type == UUIDS16_ALL) {
        for (int j = 0; j + 1 < n && a->n_uuids < TPMS_ADV_UUIDS; j += 2) {
            a->uuids16[a->n_uuids++] = (uint16_t)(v[j] | (v[j + 1] << 8));
        }
    } else if (type == NAME_SHORT || type == NAME_FULL) {
        if (!a->name[0]) {
            int m = n < (int)sizeof(a->name) - 1 ? n : (int)sizeof(a->name) - 1;
            for (int j = 0; j < m; j++) {
                a->name[j] = v[j] >= 0x20 && v[j] < 0x7F ? (char)v[j] : '?';
            }
            a->name[m] = 0;
        }
    } else if (type == SERVICE_DATA16 && a->service_uuid < 0 && n >= 2) {
        a->service_uuid = v[0] | (v[1] << 8);
        a->service_data = v + 2;
        a->service_len = n - 2;
    }
}

void tpms_adv_parse(tpms_adv_t *a, const uint8_t *raw, int len)
{
    memset(a, 0, sizeof(*a));
    a->mfr_len = -1;
    a->service_uuid = -1;
    int i = 0;
    while (i + 1 < len) {
        int n = raw[i];
        if (n == 0) {
            break;   // the zero padding some stacks add after the last structure
        }
        int start = i + 2;
        int end = i + 1 + n;
        if (end > len) {
            end = len;
            a->truncated = true;
        }
        add(a, raw[i + 1], raw + start, end - start);
        i += 1 + n;
    }
    if (len > 0 && i == len - 1 && raw[i] != 0) {
        a->truncated = true;   // a length byte with nothing after it
    }
}

int tpms_adv_company(const tpms_adv_t *a)
{
    return a->mfr_len >= 2 ? a->mfr[0] | (a->mfr[1] << 8) : -1;
}

bool tpms_adv_has_uuid(const tpms_adv_t *a, uint16_t uuid)
{
    for (int i = 0; i < a->n_uuids; i++) {
        if (a->uuids16[i] == uuid) return true;
    }
    return false;
}
