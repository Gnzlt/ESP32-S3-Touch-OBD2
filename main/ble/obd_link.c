#include <ctype.h>
#include <string.h>
#include "esp_log.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "sdkconfig.h"
#include "app.h"
#include "mem_report.h"
#include "obd_link.h"
#include "ble_scan.h"
#include "tpms/tpms_formats.h"

static const char *TAG = "link";

// The GATT layouts of BLE ELM327 adapters.
typedef struct {
    const char *name;
    uint16_t service, notify, write;   // 16-bit UUIDs; notify == write: one characteristic both ways
} profile_t;

static const profile_t PROFILES[] = {
    {"18F0", 0x18F0, 0x2AF0, 0x2AF1},   // Vgate vLinker, iCar Pro BLE / 2S (field-tested)
    {"FFF0", 0xFFF0, 0xFFF1, 0xFFF2},   // OBDLink CX, most "OBDII" BLE clones
    {"FFE0", 0xFFE0, 0xFFE1, 0xFFE1},   // HM-10 style: LELink, Veepeak, Konnwei
};
#define N_PROFILES ((int)(sizeof(PROFILES) / sizeof(PROFILES[0])))

// Advertised names of ELM327 adapters, matched as upper-case substrings when
// the adapter does not advertise its service UUID.
static const char *NAMES[] = {"VLINK", "V-LINK", "VGATE", "ICAR", "OBD", "ELM",
                              "VEEPEAK", "CARISTA", "KONNWEI", "MEATPI"};

typedef enum { L_OFF, L_WAIT_SYNC, L_BACKOFF, L_SCAN, L_CONNECT, L_SETUP, L_READY } link_state_t;

#define SCAN_MS        10000   // one adapter search window
#define CONNECT_MS     8000
#define SETUP_MS       10000   // MTU, discovery and CCCD after connecting
#define STEADY_MS      300000  // retry briskly for 5 minutes after start or a lost link
#define PAUSE_STEADY   2000
#define PAUSE_MIN      10000
#define PAUSE_MAX      60000

static QueueHandle_t s_events;
static uint8_t s_own_addr_type;
static volatile link_state_t s_state = L_OFF;
static volatile bool s_wanted;
static bool s_synced;
static int64_t s_since;              // when the state last changed
static int64_t s_search_until;       // the end of the adapter search window
static int64_t s_retry_at;
static int64_t s_steady_until;
static int s_pause = PAUSE_MIN;
static bool s_direct_next = true;    // the next attempt connects the saved adapter

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static ble_addr_t s_saved;
static bool s_have_saved;
static ble_addr_t s_peer;
static char s_name[32];

static int s_profile;                // index into PROFILES, -1 = none yet
static uint16_t s_svc_start, s_svc_end;
static uint16_t s_notify_handle, s_write_handle, s_cccd_handle;
static bool s_write_no_rsp;
static bool s_security_tried;

static int gap_event(struct ble_gap_event *ev, void *arg);

static void post(link_ev_type_t type, const uint8_t *data, int len)
{
    link_event_t e = {.type = type, .len = (uint8_t)len};
    if (len > 0) {
        memcpy(e.data, data, len);
    }
    xQueueSend(s_events, &e, pdMS_TO_TICKS(50));
}

static void post_phase(obd_phase_t phase)
{
    uint8_t p = (uint8_t)phase;
    post(LINK_EV_PHASE, &p, 1);
}

static void to(link_state_t st)
{
    s_state = st;
    s_since = app_now_ms();
}

// Pause before the next attempt: short for a while after start or after a
// lost link, then doubling to a minute so a parked bike costs little.
static void backoff(void)
{
    int64_t now = app_now_ms();
    int pause = now < s_steady_until ? PAUSE_STEADY : s_pause;
    if (now >= s_steady_until && s_pause < PAUSE_MAX) {
        s_pause = s_pause * 2 > PAUSE_MAX ? PAUSE_MAX : s_pause * 2;
    }
    s_retry_at = now + pause;
    to(L_BACKOFF);
    post_phase(PHASE_SCAN);
}

static void fail(const char *why, int rc)
{
    ESP_LOGW(TAG, "%s (rc %d)", why, rc);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);   // DISCONNECT follows
        return;
    }
    backoff();
}

// ---- Finding the adapter ------------------------------------------------

static bool name_matches(const char *name)
{
    char up[32];
    size_t n = strlen(name);
    if (n == 0) {
        return false;
    }
    if (n >= sizeof(up)) n = sizeof(up) - 1;
    for (size_t i = 0; i < n; i++) up[i] = (char)toupper((unsigned char)name[i]);
    up[n] = 0;
    const char *filter = CONFIG_DASH_ADAPTER_NAME;
    if (filter[0]) {
        char f[32];
        size_t m = strlen(filter) < sizeof(f) - 1 ? strlen(filter) : sizeof(f) - 1;
        for (size_t i = 0; i < m; i++) f[i] = (char)toupper((unsigned char)filter[i]);
        f[m] = 0;
        return strstr(up, f) != NULL;
    }
    for (size_t i = 0; i < sizeof(NAMES) / sizeof(NAMES[0]); i++) {
        if (strstr(up, NAMES[i])) {
            return true;
        }
    }
    return false;
}

// Tyre sensors advertise too, some with the FFE0 service of HM-10 style
// adapters (DJTPMS does), and were once taken for the adapter. Anything that
// decodes as a TPMS sensor, or is named like one, is not an adapter.
static bool is_tyre_sensor(const uint8_t *data, int len, const char *name)
{
    tpms_adv_t a;
    tpms_reading_t r;
    tpms_adv_parse(&a, data, len);
    if (tpms_decode(&a, &r)) {
        return true;
    }
    char up[32];
    size_t n = strlen(name);
    if (n >= sizeof(up)) n = sizeof(up) - 1;
    for (size_t i = 0; i < n; i++) up[i] = (char)toupper((unsigned char)name[i]);
    up[n] = 0;
    return strstr(up, "TPMS") != NULL;
}

static bool adv_matches(const struct ble_hs_adv_fields *f, const uint8_t *data, int len, char *name_out)
{
    name_out[0] = 0;
    if (f->name && f->name_len > 0) {
        int n = f->name_len < 31 ? f->name_len : 31;
        memcpy(name_out, f->name, n);
        name_out[n] = 0;
    }
    if (is_tyre_sensor(data, len, name_out)) {
        return false;
    }
    if (CONFIG_DASH_ADAPTER_NAME[0]) {
        return name_matches(name_out);   // a name filter is set: only that name
    }
    for (int i = 0; i < f->num_uuids16; i++) {
        for (int p = 0; p < N_PROFILES; p++) {
            if (ble_uuid_u16(&f->uuids16[i].u) == PROFILES[p].service) {
                return true;
            }
        }
    }
    return name_matches(name_out);
}

static void connect_to(const ble_addr_t *addr)
{
    s_peer = *addr;
    ble_scan_pause();   // NimBLE refuses to connect while scanning
    int rc = ble_gap_connect(s_own_addr_type, addr, CONNECT_MS, NULL, gap_event, NULL);
    if (rc != 0) {
        ble_scan_resume();
        fail("connect failed to start", rc);
        return;
    }
    to(L_CONNECT);
    post_phase(PHASE_CONNECT);
}

// The search runs on the shared scan (ble_scan.c), actively, for SCAN_MS;
// obd_link_poll ends it.
static void start_scan(void)
{
    s_search_until = app_now_ms() + SCAN_MS;
    to(L_SCAN);
    ble_scan_want_adapter(true);
    post_phase(PHASE_SCAN);
}

// Every advertisement the shared scan hears, in the NimBLE host task.
static void on_adv(const struct ble_gap_disc_desc *d)
{
    if (s_state != L_SCAN) {
        return;
    }
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0) {
        return;
    }
    char name[32];
    if (adv_matches(&f, d->data, d->length_data, name)) {
        ESP_LOGI(TAG, "found %s", name[0] ? name : "an adapter by its service");
        snprintf(s_name, sizeof(s_name), "%s", name);
        ble_scan_want_adapter(false);
        connect_to(&d->addr);
    }
}

static void attempt(void)
{
    if (s_have_saved && s_direct_next) {
        s_direct_next = false;
        ESP_LOGI(TAG, "connecting to the saved adapter");
        connect_to(&s_saved);
    } else {
        s_direct_next = true;
        start_scan();
    }
}

// ---- After connecting: MTU, discovery, subscribe ---------------------------

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg);
static int on_name_read(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg);
static void link_ready(void);

static void subscribe(void)
{
    static const uint8_t on[2] = {0x01, 0x00};   // notifications (Connect IQ never got indications either)
    int rc = ble_gattc_write_flat(s_conn, s_cccd_handle, on, sizeof(on), on_cccd_written, NULL);
    if (rc != 0) {
        fail("CCCD write failed to start", rc);
    }
}

static int on_cccd_written(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN)
        || err->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC)) {
        if (!s_security_tried) {
            s_security_tried = true;
            ESP_LOGI(TAG, "adapter wants encryption: pairing");
            ble_gap_security_initiate(conn);   // ENC_CHANGE retries the write
            return 0;
        }
    }
    if (err->status != 0) {
        fail("CCCD write refused", err->status);
        return 0;
    }
    // A name from the scan, or saved with this adapter's address (a reconnect
    // to the saved address skips the scan).
    bool same = s_have_saved && memcmp(&s_saved, &s_peer, sizeof(s_peer)) == 0;
    if (!s_name[0] && !(same && nvs_get_blob_("adapter_nm", s_name, sizeof(s_name)))) {
        s_name[0] = 0;
        // The Vgate is found by its service in its first advertisement and
        // its name only comes in the scan response, after the connect, so
        // it may still have none. Ask the adapter itself (GAP Device Name).
        int rc = ble_gattc_read_by_uuid(conn, 1, 0xFFFF, BLE_UUID16_DECLARE(BLE_SVC_GAP_CHR_UUID16_DEVICE_NAME),
                                        on_name_read, NULL);
        if (rc == 0) return 0;
    }
    link_ready();
    return 0;
}

// The GAP Device Name read: each match, then EDONE (or an error). A missing
// name never stops the link.
static int on_name_read(uint16_t conn, const struct ble_gatt_error *err, struct ble_gatt_attr *attr, void *arg)
{
    if (err->status == 0 && attr && attr->om) {
        if (!s_name[0]) {
            uint16_t n = OS_MBUF_PKTLEN(attr->om);
            if (n > sizeof(s_name) - 1) n = sizeof(s_name) - 1;
            if (os_mbuf_copydata(attr->om, 0, n, s_name) == 0) s_name[n] = 0;
            else s_name[0] = 0;
        }
        return 0;
    }
    if (s_state == L_SETUP && s_conn == conn) link_ready();   // not after a disconnect
    return 0;
}

static void link_ready(void)
{
    if (s_name[0]) nvs_set_blob_("adapter_nm", s_name, sizeof(s_name));
    s_name[sizeof(s_name) - 1] = 0;
    ESP_LOGI(TAG, "ready: %s, profile %s, MTU %d, %s", s_name[0] ? s_name : "(no name)",
             PROFILES[s_profile].name, ble_att_mtu(s_conn), s_write_no_rsp ? "write-no-rsp" : "write");
    s_saved = s_peer;
    s_have_saved = true;
    nvs_set_blob_("adapter", &s_saved, sizeof(s_saved));
    s_pause = PAUSE_MIN;
    to(L_READY);
    post(LINK_EV_UP, NULL, 0);
}

static int on_dsc(uint16_t conn, const struct ble_gatt_error *err, uint16_t chr_val_handle,
                  const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0) {
        if (s_cccd_handle == 0 && dsc->handle > s_notify_handle
            && ble_uuid_cmp(&dsc->uuid.u, BLE_UUID16_DECLARE(BLE_GATT_DSC_CLT_CFG_UUID16)) == 0) {
            s_cccd_handle = dsc->handle;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (s_cccd_handle == 0) {
            s_cccd_handle = s_notify_handle + 1;   // where every adapter seen so far has it
        }
        subscribe();
        return 0;
    }
    fail("descriptor discovery failed", err->status);
    return 0;
}

static int on_chr(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_chr *chr, void *arg)
{
    const profile_t *p = &PROFILES[s_profile];
    if (err->status == 0) {
        uint16_t u = chr->uuid.u.type == BLE_UUID_TYPE_16 ? ble_uuid_u16(&chr->uuid.u) : 0;
        if (u == p->notify && (chr->properties & (BLE_GATT_CHR_PROP_NOTIFY | BLE_GATT_CHR_PROP_INDICATE))) {
            s_notify_handle = chr->val_handle;
        }
        if (u == p->write && (chr->properties & (BLE_GATT_CHR_PROP_WRITE | BLE_GATT_CHR_PROP_WRITE_NO_RSP))) {
            s_write_handle = chr->val_handle;
            s_write_no_rsp = (chr->properties & BLE_GATT_CHR_PROP_WRITE_NO_RSP) != 0;
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (!s_notify_handle || !s_write_handle) {
            fail("the adapter's service lacks its characteristics", 0);
            return 0;
        }
        int rc = ble_gattc_disc_all_dscs(conn, s_notify_handle, s_svc_end, on_dsc, NULL);
        if (rc != 0) fail("descriptor discovery failed to start", rc);
        return 0;
    }
    fail("characteristic discovery failed", err->status);
    return 0;
}

static int on_svc(uint16_t conn, const struct ble_gatt_error *err, const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0) {
        if (svc->uuid.u.type == BLE_UUID_TYPE_16) {
            uint16_t u = ble_uuid_u16(&svc->uuid.u);
            for (int p = 0; p < N_PROFILES; p++) {
                // The first profile in the list wins: the Vgate's 18F0 over its vendor extras.
                if (u == PROFILES[p].service && (s_profile < 0 || p < s_profile)) {
                    s_profile = p;
                    s_svc_start = svc->start_handle;
                    s_svc_end = svc->end_handle;
                }
            }
        }
        return 0;
    }
    if (err->status == BLE_HS_EDONE) {
        if (s_profile < 0) {
            fail("no ELM327 service on this device", 0);
            return 0;
        }
        int rc = ble_gattc_disc_all_chrs(conn, s_svc_start, s_svc_end, on_chr, NULL);
        if (rc != 0) fail("characteristic discovery failed to start", rc);
        return 0;
    }
    fail("service discovery failed", err->status);
    return 0;
}

static int on_mtu(uint16_t conn, const struct ble_gatt_error *err, uint16_t mtu, void *arg)
{
    // Discovery goes on whether or not the adapter took the larger MTU.
    int rc = ble_gattc_disc_all_svcs(conn, on_svc, NULL);
    if (rc != 0) fail("service discovery failed to start", rc);
    return 0;
}

// ---- GAP events -----------------------------------------------------------

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        ble_scan_resume();
        if (ev->connect.status != 0) {
            ESP_LOGI(TAG, "connect failed (%d)", ev->connect.status);
            s_conn = BLE_HS_CONN_HANDLE_NONE;
            backoff();
            return 0;
        }
        s_conn = ev->connect.conn_handle;
        ble_scan_set_connected(true);
        s_profile = -1;
        s_notify_handle = s_write_handle = s_cccd_handle = 0;
        s_security_tried = false;
        to(L_SETUP);
        if (ble_gattc_exchange_mtu(s_conn, on_mtu, NULL) != 0) {
            on_mtu(s_conn, NULL, 23, NULL);
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        bool was_ready = s_state == L_READY;
        ESP_LOGI(TAG, "disconnected (reason 0x%x)", ev->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        ble_scan_set_connected(false);
        if (was_ready) {
            post(LINK_EV_DOWN, NULL, 0);
            // Mid-ride the bike is right there: try the same adapter at once.
            s_steady_until = app_now_ms() + STEADY_MS;
            s_direct_next = true;
        }
        if (!s_wanted) {
            to(L_OFF);
            return 0;
        }
        backoff();
        if (was_ready) {
            s_retry_at = app_now_ms();
        }
        return 0;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        if (ev->enc_change.status == 0 && s_state == L_SETUP && s_cccd_handle) {
            subscribe();
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX:
        if (ev->notify_rx.attr_handle == s_notify_handle && s_state == L_READY) {
            uint8_t buf[256];
            uint16_t len = 0;
            if (ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, sizeof(buf), &len) == 0 || len > 0) {
                for (int i = 0; i < len; i += LINK_EV_MAX) {
                    int n = len - i < LINK_EV_MAX ? len - i : LINK_EV_MAX;
                    post(LINK_EV_DATA, buf + i, n);
                }
            }
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU %d", ev->mtu.value);
        return 0;

    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        // The adapter forgot us: drop the old bond and pair again.
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }
    }
    return 0;
}

// ---- Host task and public API ----------------------------------------------

static void on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &s_own_addr_type);
    s_synced = true;
    ble_scan_ready(s_own_addr_type);
    if (s_state == L_WAIT_SYNC) {
        s_retry_at = 0;
        to(L_BACKOFF);
    }
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE reset (%d)", reason);
    s_synced = false;
    ble_scan_lost();
}

static void host_task(void *param)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void obd_link_init(QueueHandle_t events)
{
    s_events = events;
    s_have_saved = nvs_get_blob_("adapter", &s_saved, sizeof(s_saved));
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %s", esp_err_to_name(err));
        post_phase(PHASE_NO_BLE);
        return;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_scan_set_adapter_sink(on_adv);
    nimble_port_freertos_init(host_task);
    mem_checkpoint("ble up");
}

void obd_link_start(void)
{
    s_wanted = true;
    s_steady_until = app_now_ms() + STEADY_MS;
    s_direct_next = true;
    s_pause = PAUSE_MIN;
    if (s_state == L_OFF) {
        s_retry_at = 0;
        to(s_synced ? L_BACKOFF : L_WAIT_SYNC);
    }
    post_phase(PHASE_SCAN);
}

void obd_link_stop(void)
{
    s_wanted = false;
    ble_scan_want_adapter(false);
    if (s_state == L_CONNECT) {
        ble_gap_conn_cancel();
        ble_scan_resume();
    }
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    } else {
        to(L_OFF);
    }
}

void obd_link_poll(int64_t now)
{
    ble_scan_update();
    switch (s_state) {
    case L_SCAN:
        if (now >= s_search_until) {
            ble_scan_want_adapter(false);
            backoff();   // the window ended with nothing found
        }
        break;
    case L_BACKOFF:
        if (s_wanted && s_synced && now >= s_retry_at) {
            attempt();
        }
        break;
    case L_SETUP:
        if (now - s_since > SETUP_MS) {
            fail("setup took too long", 0);
        }
        break;
    case L_CONNECT:
        // ble_gap_connect's own timeout reports through CONNECT; this only
        // catches a controller that never answers.
        if (now - s_since > CONNECT_MS + 3000) {
            ble_gap_conn_cancel();
            ble_scan_resume();
            backoff();
        }
        break;
    default:
        break;
    }
}

bool obd_link_send(const uint8_t *data, int len)
{
    if (s_state != L_READY || s_conn == BLE_HS_CONN_HANDLE_NONE) {
        return false;
    }
    int chunk = ble_att_mtu(s_conn) - 3;
    if (chunk < 20) chunk = 20;
    for (int i = 0; i < len; i += chunk) {
        int n = len - i < chunk ? len - i : chunk;
        int rc = s_write_no_rsp ? ble_gattc_write_no_rsp_flat(s_conn, s_write_handle, data + i, n)
                                : ble_gattc_write_flat(s_conn, s_write_handle, data + i, n, NULL, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "write failed (%d)", rc);
            return false;
        }
    }
    return true;
}

void obd_link_restart(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
}

void obd_link_forget(void)
{
    s_have_saved = false;
    memset(&s_saved, 0, sizeof(s_saved));
    nvs_erase_("adapter");
    nvs_erase_("adapter_nm");
}

const char *obd_link_name(void)
{
    return s_name;
}
