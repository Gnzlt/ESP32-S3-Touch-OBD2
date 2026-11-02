#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_app_desc.h"
#include "esp_vfs_fat.h"
#include "bsp/esp-bsp.h"
#include "app.h"
#include "sd_log.h"

static const char *TAG = "sdlog";

#define ROOT          BSP_SD_MOUNT_POINT "/obd2"
#define RAW_BUF       (32 * 1024)
#define TRIP_BUF      (16 * 1024)
#define EVENT_BUF     (2 * 1024)
#define SYNC_MS       5000
#define SUMMARY_MS    30000
#define REMOUNT_MS    30000
#define CHUNK         2048
#define PRE_RIDE      (16 * 1024)   // raw traffic kept from before a ride opens

const char *SD_TRIP_HEADER =
    "t_s,rpm,kmh,gear,throttle_pct,load_pct,map_kpa,coolant_c,iat_c,advance_deg,stft_pct,fuel_sys,fuel_lph_raw,fuel_ml_raw,volts,"
    "lean_deg,lat_g,lon_g,trip_km,board_soc,board_mv,front_kpa,front_c,rear_kpa,rear_c,chip_c,cell_c,board_ma";

const char *SD_EVENT_HEADER = "t_s,type,detail";

// time (UTC, empty with the clock unset) is last, so rows written before it
// existed still line up.
const char *SD_FILL_HEADER =
    "fill,ride,uptime_s,km_since_last,raw_l_since_last,litres_pumped,k,model,coverage_pct,time";

#define FILL_PENDING 4
static EXT_RAM_BSS_ATTR char s_fill_rows[FILL_PENDING][160];   // guarded by s_mutex
static int s_fill_n;

// tpms.cfg both ways, guarded by s_mutex: the text to write (pending), and
// the text the last mount read.
static EXT_RAM_BSS_ATTR char s_tpms_out[SD_TPMS_CFG_MAX], s_tpms_in[SD_TPMS_CFG_MAX];
static bool s_tpms_out_pending;
static uint32_t s_tpms_in_seq;

static StreamBufferHandle_t s_raw, s_trip, s_events;
static SemaphoreHandle_t s_mutex;      // s_status, s_summary and the request flags
static sd_status_t s_status;
static EXT_RAM_BSS_ATTR char s_summary[SD_SUMMARY_MAX];
static bool s_summary_dirty;
static volatile bool s_want_begin, s_want_end, s_want_format;
static volatile bool s_want_shutdown, s_shut_down;
static uint32_t s_dropped;

// Before a ride opens, raw lines go to an overwrite-oldest ring instead, so
// the ride's log can start with how it got there: the BLE connect, the ELM
// init, protocol detection and the ECU lock. s_ring_mutex guards the ring and
// s_raw_live, which says where raw lines go.
static SemaphoreHandle_t s_ring_mutex;
static char *s_ring;
static size_t s_ring_head, s_ring_len;
static bool s_raw_live;

// The Rides page's list, guarded by s_mutex. Each read fills s_rides_new,
// then swaps it in, so a copy never sees half a read.
static ride_summary_t *s_rides, *s_rides_new;
static int s_rides_n = -1;
static uint32_t s_rides_seq;
static volatile bool s_want_rides;

static FILE *s_f_raw, *s_f_trip, *s_f_events;
static char s_dir[64];

static void list_card(void);

// ---- Producers (other tasks) ---------------------------------------------------

static void push(StreamBufferHandle_t sb, const char *text, size_t len)
{
    if (!sb || !s_status.logging) {
        return;
    }
    // Never block a producer: if the card can't keep up, drop and count it.
    if (xStreamBufferSpacesAvailable(sb) < len || xStreamBufferSend(sb, text, len, 0) != len) {
        s_dropped++;
    }
}

static void ring_put(const char *text, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        s_ring[(s_ring_head + s_ring_len) % PRE_RIDE] = text[i];
        if (s_ring_len < PRE_RIDE) {
            s_ring_len++;
        } else {
            s_ring_head = (s_ring_head + 1) % PRE_RIDE;
        }
    }
}

static void push_raw(const char *text, size_t len)
{
    if (!s_ring_mutex) {
        return;
    }
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    if (s_raw_live) {
        push(s_raw, text, len);
    } else if (s_ring) {
        ring_put(text, len);
    }
    xSemaphoreGive(s_ring_mutex);
}

void sd_log_raw(char dir, const uint8_t *data, int len, int64_t now_ms)
{
    // One line per non-empty line of the message: "   12.345 > 010C0D1".
    char line[160];
    int s = 0;
    while (s < len) {
        int e = s;
        while (e < len && data[e] != '\r' && data[e] != '\n') e++;
        if (e > s) {
            int n = snprintf(line, sizeof(line), "%10.3f %c %.*s\n", now_ms / 1000.0, dir,
                             (e - s) > 120 ? 120 : (e - s), (const char *)data + s);
            push_raw(line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
        }
        s = e + 1;
    }
}

void sd_log_note(const char *text, int64_t now_ms)
{
    char line[160];
    int n = snprintf(line, sizeof(line), "%10.3f # %s\n", now_ms / 1000.0, text);
    push_raw(line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
}

void sd_log_trip_row(const char *row)
{
    char line[256];
    int n = snprintf(line, sizeof(line), "%s\n", row);
    push(s_trip, line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
}

void sd_log_event(double t_s, const char *type, const char *detail)
{
    char line[96];
    int n = snprintf(line, sizeof(line), "%.1f,%s,%s\n", t_s, type, detail);
    push(s_events, line, n < (int)sizeof(line) ? n : (int)sizeof(line) - 1);
}

void sd_log_summary(const char *text)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_summary, sizeof(s_summary), "%s", text);
    s_summary_dirty = true;
    xSemaphoreGive(s_mutex);
}

void sd_log_fillup(const char *row)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_fill_n < FILL_PENDING) {
        snprintf(s_fill_rows[s_fill_n++], sizeof(s_fill_rows[0]), "%s", row);
    }
    xSemaphoreGive(s_mutex);
}

void sd_log_tpms_cfg_put(const char *text)
{
    if (!s_mutex) return;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_tpms_out, sizeof(s_tpms_out), "%s", text);
    s_tpms_out_pending = true;
    xSemaphoreGive(s_mutex);
}

bool sd_log_tpms_cfg_get(char *out, size_t n, uint32_t *seq)
{
    if (!s_mutex) return false;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool fresh = s_tpms_in_seq != *seq;
    if (fresh) {
        snprintf(out, n, "%s", s_tpms_in);
        *seq = s_tpms_in_seq;
    }
    xSemaphoreGive(s_mutex);
    return fresh;
}

void sd_log_format(void) { s_want_format = true; }
void sd_log_rides_refresh(void) { s_want_rides = true; }

int sd_log_rides(ride_summary_t *out, int max, uint32_t *seq)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_rides_n < max ? s_rides_n : max;
    if (n > 0) memcpy(out, s_rides, n * sizeof(*out));
    if (seq) *seq = s_rides_seq;
    xSemaphoreGive(s_mutex);
    return n;
}
static int64_t s_ride_t0_ms;
void sd_log_ride_begin(int64_t start_ms)
{
    s_ride_t0_ms = start_ms;
    s_want_begin = true;
}
void sd_log_ride_end(void) { s_want_end = true; }
void sd_log_shutdown(void) { s_want_shutdown = true; }
bool sd_log_is_shut_down(void) { return s_shut_down; }

void sd_log_status(sd_status_t *out)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_status;
    xSemaphoreGive(s_mutex);
}

// ---- Writer task --------------------------------------------------------------

static void set_error(const char *e)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    snprintf(s_status.error, sizeof(s_status.error), "%s", e);
    xSemaphoreGive(s_mutex);
    if (e[0]) ESP_LOGW(TAG, "%s", e);
}

static void update_space(void)
{
    uint64_t total = 0, free_b = 0;
    if (esp_vfs_fat_info(BSP_SD_MOUNT_POINT, &total, &free_b) == ESP_OK) {
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.total_mb = (uint32_t)(total / (1024 * 1024));
        s_status.free_mb = (uint32_t)(free_b / (1024 * 1024));
        xSemaphoreGive(s_mutex);
    }
}

#define TPMS_CFG_PATH ROOT "/tpms.cfg"

// At each mount and after a format: what the card says about the tyre sensors.
static void read_tpms_cfg(void)
{
    static EXT_RAM_BSS_ATTR char text[SD_TPMS_CFG_MAX];
    size_t len = 0;
    FILE *f = fopen(TPMS_CFG_PATH, "r");
    if (f) {
        len = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
    }
    text[len] = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(s_tpms_in, text, len + 1);
    s_tpms_in_seq++;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "tpms.cfg: %s", f ? "read" : "none on the card");
}

static void write_tpms_cfg(void)
{
    static EXT_RAM_BSS_ATTR char text[SD_TPMS_CFG_MAX];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memcpy(text, s_tpms_out, sizeof(text));
    s_tpms_out_pending = false;
    xSemaphoreGive(s_mutex);
    FILE *f = fopen(TPMS_CFG_PATH, "w");
    if (!f || fputs(text, f) < 0) {
        set_error("can't write tpms.cfg");
    } else {
        ESP_LOGI(TAG, "tpms.cfg written");
    }
    if (f) fclose(f);
}

static bool mount(void)
{
    if (bsp_sdcard_mount() != ESP_OK) {
        set_error("no card");
        return false;
    }
    mkdir(ROOT, 0775);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.mounted = true;
    xSemaphoreGive(s_mutex);
    set_error("");
    update_space();
    ESP_LOGI(TAG, "card mounted: %lu MB free of %lu MB", (unsigned long)s_status.free_mb,
             (unsigned long)s_status.total_mb);
    list_card();
    read_tpms_cfg();
    return true;
}

// The highest ride folder on the card, 0 for none.
static int last_ride_on_card(void)
{
    int max = 0;
    DIR *d = opendir(ROOT);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) != NULL) {
            int n;
            if (sscanf(e->d_name, "ride_%d", &n) == 1 && n > max) max = n;
        }
        closedir(d);
    }
    return max;
}

// The next ride number: one more than the highest folder on the card, and
// never below NVS "ride_next", so numbers carry on after a format and a ride
// downloaded before it can't be mistaken for a new one.
static int next_ride(void)
{
    int saved = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_get_blob_("ride_next", &saved, sizeof(saved));
    xSemaphoreGive(s_mutex);
    int card = last_ride_on_card() + 1;
    return card > saved ? card : saved;
}

static void save_next_ride(int next)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int saved = 0;
    nvs_get_blob_("ride_next", &saved, sizeof(saved));
    if (next > saved) nvs_set_blob_("ride_next", &next, sizeof(next));
    xSemaphoreGive(s_mutex);
}

int sd_log_raise_next_ride(int at_least)
{
    save_next_ride(at_least);
    int saved = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    nvs_get_blob_("ride_next", &saved, sizeof(saved));
    xSemaphoreGive(s_mutex);
    return saved;
}

static FILE *open_in_dir(const char *name)
{
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", s_dir, name);
    FILE *f = fopen(path, "a");
    if (f) {
        setvbuf(f, NULL, _IOFBF, 8192);
    }
    return f;
}

static void close_files(void)
{
    if (s_f_raw) { fclose(s_f_raw); s_f_raw = NULL; }
    if (s_f_trip) { fclose(s_f_trip); s_f_trip = NULL; }
    if (s_f_events) { fclose(s_f_events); s_f_events = NULL; }
}

// ride.json goes to ride.json.tmp first, then replaces the old one (FAT
// can't rename over a file). A power cut leaves at least one whole copy:
// the readers fall back to the .tmp when ride.json is missing or cut short.
static void write_summary(void)
{
    static EXT_RAM_BSS_ATTR char text[sizeof(s_summary)];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool dirty = s_summary_dirty;
    if (dirty) {
        memcpy(text, s_summary, sizeof(text));
        s_summary_dirty = false;
    }
    xSemaphoreGive(s_mutex);
    if (!dirty || !s_dir[0]) {
        return;
    }
    char path[96], tmp[100];
    snprintf(path, sizeof(path), "%s/ride.json", s_dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    bool ok = fputs(text, f) >= 0;
    ok = fflush(f) == 0 && fsync(fileno(f)) == 0 && ok;
    fclose(f);
    if (ok) {
        unlink(path);
        rename(tmp, path);
    }
}

static void begin_ride(void)
{
    close_files();
    int n = next_ride();
    snprintf(s_dir, sizeof(s_dir), "%s/ride_%04d", ROOT, n);
    if (mkdir(s_dir, 0775) != 0) {
        set_error("can't create ride folder");
        s_dir[0] = 0;
        return;
    }
    save_next_ride(n + 1);
    s_f_raw = open_in_dir("obd_raw.log");
    s_f_trip = open_in_dir("trip.csv");
    s_f_events = open_in_dir("events.csv");
    if (!s_f_raw || !s_f_trip || !s_f_events) {
        close_files();
        set_error("can't open log files");
        return;
    }
    fprintf(s_f_raw, "# Raw ELM327 log, ride %d. Columns: seconds since boot, direction "
                     "('>' sent to the adapter, '<' received), the line as on the wire. "
                     "t_s 0 in trip.csv and events.csv is %.3f s here.\n", n, s_ride_t0_ms / 1000.0);
    fprintf(s_f_trip, "%s\n", SD_TRIP_HEADER);
    fprintf(s_f_events, "%s\n", SD_EVENT_HEADER);
    xStreamBufferReset(s_raw);
    xStreamBufferReset(s_trip);
    xStreamBufferReset(s_events);
    // Take the pre-ride history and switch raw lines to the live stream in
    // one step, so no line falls between the two.
    char *pre = heap_caps_malloc(PRE_RIDE, MALLOC_CAP_SPIRAM);
    size_t pre_len = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.logging = true;
    xSemaphoreGive(s_mutex);
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    if (pre && s_ring) {
        for (size_t i = 0; i < s_ring_len; i++) pre[i] = s_ring[(s_ring_head + i) % PRE_RIDE];
        pre_len = s_ring_len;
        // A full ring has lost the start of its oldest line: begin at the next one.
        size_t skip = 0;
        if (s_ring_len == PRE_RIDE) {
            while (skip < pre_len && pre[skip] != '\n') skip++;
            skip = skip < pre_len ? skip + 1 : pre_len;
        }
        memmove(pre, pre + skip, pre_len - skip);
        pre_len -= skip;
    }
    s_ring_head = s_ring_len = 0;
    s_raw_live = true;
    xSemaphoreGive(s_ring_mutex);
    if (pre_len) {
        fputs("# --- before the ride started ---\n", s_f_raw);
        fwrite(pre, 1, pre_len, s_f_raw);
        fputs("# --- ride started ---\n", s_f_raw);
    }
    free(pre);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.ride = n;
    s_status.written_kb = 0;
    xSemaphoreGive(s_mutex);
    s_dropped = 0;
    ESP_LOGI(TAG, "logging ride %d to %s", n, s_dir);
}

static void raw_to_ring(void)
{
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    s_raw_live = false;
    xSemaphoreGive(s_ring_mutex);
}

static void end_ride(void)
{
    raw_to_ring();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.logging = false;
    xSemaphoreGive(s_mutex);
    write_summary();
    close_files();
    if (s_dir[0]) ESP_LOGI(TAG, "ride closed (%s)", s_dir);
    s_dir[0] = 0;
    update_space();
    s_want_rides = true;   // its final summary
}

// A write failed: the card is gone or full. Close up and try mounting later.
static void card_lost(void)
{
    raw_to_ring();
    close_files();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.logging = false;
    s_status.mounted = false;
    xSemaphoreGive(s_mutex);
    s_dir[0] = 0;
    bsp_sdcard_unmount();
    set_error("write failed: card removed or full");
}

static size_t drain(StreamBufferHandle_t sb, FILE *f, uint8_t *buf)
{
    size_t total = 0, n;
    while ((n = xStreamBufferReceive(sb, buf, CHUNK, 0)) > 0) {
        if (f && fwrite(buf, 1, n, f) != n) {
            card_lost();
            return 0;
        }
        total += n;
    }
    return total;
}

// Log what's on the card at mount: the top level, then the ride folders with
// their file sizes, and the first lines of the newest ride's files.
static void list_card(void)
{
    DIR *d = opendir(BSP_SD_MOUNT_POINT);
    if (!d) return;
    struct dirent *e;
    int n = 0;
    while ((e = readdir(d)) != NULL && n < 40) {
        ESP_LOGI(TAG, "  /%s%s", e->d_name, e->d_type == DT_DIR ? "/" : "");
        n++;
    }
    closedir(d);
    if (n == 0) ESP_LOGI(TAG, "  (card is empty)");
    int last = last_ride_on_card();
    for (int r = last > 5 ? last - 4 : 1; r <= last; r++) {
        const char *files[] = {"trip.csv", "events.csv", "obd_raw.log", "ride.json"};
        for (int i = 0; i < 4; i++) {
            char path[96];
            snprintf(path, sizeof(path), "%s/ride_%04d/%s", ROOT, r, files[i]);
            struct stat st;
            if (stat(path, &st) != 0) continue;
            ESP_LOGI(TAG, "  ride_%04d/%-12s %8ld bytes", r, files[i], (long)st.st_size);
            if (r != last) continue;
            FILE *f = fopen(path, "r");
            char line[200];
            for (int k = 0; f && k < 3 && fgets(line, sizeof(line), f); k++) {
                line[strcspn(line, "\r\n")] = 0;
                ESP_LOGI(TAG, "      | %s", line);
            }
            if (f) fclose(f);
        }
    }
}

static void do_format(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.formatting = true;
    xSemaphoreGive(s_mutex);
    if (s_status.logging) end_ride();
    esp_err_t err = ESP_OK;
    if (!s_status.mounted) {
        // An unreadable card (no FAT, exFAT): mount it with format-on-fail.
        esp_vfs_fat_sdmmc_mount_config_t mc = {.format_if_mount_failed = true, .max_files = 10,
                                                .allocation_unit_size = 32 * 1024};
        bsp_sdcard_cfg_t cfg = {.mount = &mc};
        err = bsp_sdcard_sdmmc_mount(&cfg);
    }
    if (err == ESP_OK) {
        // 32 KB clusters: the usual FAT32 choice for a 32 GB card, and fewer FAT writes.
        esp_vfs_fat_mount_config_t fc = {.format_if_mount_failed = false, .max_files = 10,
                                         .allocation_unit_size = 32 * 1024};
        ESP_LOGW(TAG, "formatting the card...");
        err = esp_vfs_fat_sdcard_format_cfg(BSP_SD_MOUNT_POINT, bsp_sdcard_get_handle(), &fc);
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.formatting = false;
    s_status.mounted = err == ESP_OK;
    if (err == ESP_OK) s_status.formats++;
    xSemaphoreGive(s_mutex);
    if (err == ESP_OK) {
        mkdir(ROOT, 0775);
        set_error("");
        update_space();
        ESP_LOGW(TAG, "card formatted: %lu MB free", (unsigned long)s_status.free_mb);
        read_tpms_cfg();   // none now: the tyre service writes its own back
    } else {
        set_error("format failed");
        ESP_LOGE(TAG, "format failed: %s", esp_err_to_name(err));
    }
}

// A ride's details: ride.json, or its .tmp if a power cut caught the rename.
static bool read_ride(int ride, char *text, size_t n, ride_summary_t *out)
{
    static const char *const NAMES[] = {"ride.json", "ride.json.tmp"};
    for (int i = 0; i < 2; i++) {
        char path[96];
        snprintf(path, sizeof(path), "%s/ride_%04d/%s", ROOT, ride, NAMES[i]);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t len = fread(text, 1, n - 1, f);
        fclose(f);
        text[len] = 0;
        if (ride_summary_parse(text, out)) return true;
    }
    return false;
}

// The newest SD_RIDES_MAX ride folders that have a summary, newest first.
static void scan_rides(void)
{
    int64_t t0 = app_now_ms();
    int nums[SD_RIDES_MAX], k = 0;
    DIR *d = s_status.mounted && s_rides_new ? opendir(ROOT) : NULL;
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        int r;
        if (sscanf(e->d_name, "ride_%d", &r) != 1 || (k == SD_RIDES_MAX && r <= nums[k - 1])) continue;
        // Insert into the sorted top list, dropping the oldest when it's full.
        int i = k < SD_RIDES_MAX ? k++ : k - 1;
        for (; i > 0 && nums[i - 1] < r; i--) nums[i] = nums[i - 1];
        nums[i] = r;
    }
    if (d) closedir(d);
    static EXT_RAM_BSS_ATTR char text[SD_SUMMARY_MAX + 1];
    int n = 0;
    for (int i = 0; i < k; i++) {
        // None yet when the ride opened less than 30 s ago.
        if (read_ride(nums[i], text, sizeof(text), &s_rides_new[n])) {
            s_rides_new[n++].ride = nums[i];
        }
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    ride_summary_t *old = s_rides;
    s_rides = s_rides_new;
    s_rides_new = old;
    s_rides_n = s_rides ? n : 0;
    s_rides_seq++;
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "rides: %d summaries in %ld ms", n, (long)(app_now_ms() - t0));
}

// Fill-ups go to one file for the whole card, not a ride folder: each row
// spans the rides since the previous fill-up.
static void write_fillups(void)
{
    char rows[FILL_PENDING][160];
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int n = s_fill_n;
    memcpy(rows, s_fill_rows, sizeof(rows));
    s_fill_n = 0;
    xSemaphoreGive(s_mutex);
    if (n == 0) {
        return;
    }
    const char *path = ROOT "/fillups.csv";
    struct stat st;
    bool fresh = stat(path, &st) != 0 || st.st_size == 0;
    FILE *f = fopen(path, "a");
    if (!f) {
        set_error("can't write fillups.csv");
        return;
    }
    if (fresh) fprintf(f, "%s\n", SD_FILL_HEADER);
    for (int i = 0; i < n; i++) fprintf(f, "%s\n", rows[i]);
    fclose(f);
    ESP_LOGI(TAG, "fill-up logged: %s", rows[n - 1]);
}

static void shut_down(uint8_t *buf)
{
    if (s_status.mounted) {
        if (s_fill_n) write_fillups();
        if (s_status.logging) drain(s_raw, s_f_raw, buf);
        if (s_status.logging) drain(s_trip, s_f_trip, buf);
        if (s_status.logging) drain(s_events, s_f_events, buf);
        if (s_status.logging) end_ride();   // the last ride.json; fclose flushes
    }
    if (s_status.mounted) {   // a failed drain unmounts it already
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.mounted = false;
        xSemaphoreGive(s_mutex);
        bsp_sdcard_unmount();
        ESP_LOGI(TAG, "card unmounted: the board is switching off");
    }
}

static void writer_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    int64_t last_sync = 0, last_summary = 0, last_mount_try = -REMOUNT_MS;
    bool resume_after_remount = false;
#if CONFIG_DASH_FORMAT_SD_ON_BOOT
    // One format per firmware build: the marker holds this build's ELF hash,
    // so a reboot of the same build doesn't erase the card again.
    char sha[17] = "", done[17] = "";
    esp_app_get_elf_sha256(sha, sizeof(sha));
    if (!nvs_get_blob_("sdfmt", done, sizeof(done)) || strcmp(done, sha) != 0) {
        last_mount_try = app_now_ms();
        mount();   // lists what is about to be erased
        do_format();
        if (s_status.formats) nvs_set_blob_("sdfmt", sha, sizeof(sha));
    }
#endif
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(200));
        int64_t now = app_now_ms();
        if (s_want_shutdown) {
            shut_down(buf);
            s_shut_down = true;
            vTaskSuspend(NULL);
        }
        if (s_want_format) {
            s_want_format = false;
            do_format();
            s_want_rides = true;
            continue;
        }
        if (s_want_rides) {
            s_want_rides = false;
            scan_rides();
        }
        if (!s_status.mounted) {
            if (now - last_mount_try >= REMOUNT_MS) {
                last_mount_try = now;
                if (mount() && resume_after_remount) {
                    resume_after_remount = false;
                    s_want_begin = true;
                }
            }
            if (s_want_begin) resume_after_remount = true;
            s_want_begin = s_want_end = false;
            continue;
        }
        if (s_fill_n) {
            write_fillups();
        }
        if (s_tpms_out_pending) {
            write_tpms_cfg();
        }
        if (s_want_end) {
            s_want_end = false;
            if (s_status.logging) end_ride();
        }
        if (s_want_begin) {
            s_want_begin = false;
            if (!s_status.logging) begin_ride();
        }
        if (!s_status.logging) {
            continue;
        }
        size_t n = drain(s_raw, s_f_raw, buf);
        if (s_status.logging) n += drain(s_trip, s_f_trip, buf);
        if (s_status.logging) n += drain(s_events, s_f_events, buf);
        if (!s_status.logging) {
            resume_after_remount = true;
            continue;
        }
        xSemaphoreTake(s_mutex, portMAX_DELAY);
        s_status.written_kb += n / 1024 + (n % 1024 ? 1 : 0);
        xSemaphoreGive(s_mutex);
        if (now - last_sync >= SYNC_MS) {
            last_sync = now;
            if (s_dropped) {
                fprintf(s_f_raw, "%10.3f # %lu lines dropped: the card was too slow\n", now / 1000.0,
                        (unsigned long)s_dropped);
                s_dropped = 0;
            }
            // fflush hands the data to FATFS; fsync commits it to the card.
            if (fflush(s_f_raw) != 0 || fflush(s_f_trip) != 0 || fflush(s_f_events) != 0
                || fsync(fileno(s_f_raw)) != 0 || fsync(fileno(s_f_trip)) != 0 || fsync(fileno(s_f_events)) != 0) {
                card_lost();
                resume_after_remount = true;
                continue;
            }
        }
        if (now - last_summary >= SUMMARY_MS) {
            last_summary = now;
            write_summary();
            update_space();
        }
    }
}

void sd_log_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_ring = heap_caps_malloc(PRE_RIDE, MALLOC_CAP_SPIRAM);
    s_ring_mutex = xSemaphoreCreateMutex();
    s_rides = heap_caps_calloc(SD_RIDES_MAX, sizeof(ride_summary_t), MALLOC_CAP_SPIRAM);
    s_rides_new = heap_caps_calloc(SD_RIDES_MAX, sizeof(ride_summary_t), MALLOC_CAP_SPIRAM);
    // The buffers live in PSRAM: the card writes are slow, the data is not.
    s_raw = xStreamBufferCreateWithCaps(RAW_BUF, 1, MALLOC_CAP_SPIRAM);
    s_trip = xStreamBufferCreateWithCaps(TRIP_BUF, 1, MALLOC_CAP_SPIRAM);
    s_events = xStreamBufferCreateWithCaps(EVENT_BUF, 1, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCore(writer_task, "sdlog", 6144, NULL, 2, NULL, 0);   // 3.5 KB used reading the ride summaries
}
