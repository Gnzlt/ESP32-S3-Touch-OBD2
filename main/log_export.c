#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "mbedtls/base64.h"
#include "bsp/esp-bsp.h"
#include "log_export.h"
#include "sd_log.h"
#include "sensors/wallclock.h"

static const char *TAG = "export";

#define ROOT     BSP_SD_MOUNT_POINT "/obd2"
#define RAW_LINE 57    // bytes per base64 line (76 characters)
#define READ_BUF (RAW_LINE * 64)

// Straight to the USB driver: printf also feeds UART0 at 115200, which would
// cap the transfer at ~11 KB/s.
static void out(const char *s, size_t n)
{
    usb_serial_jtag_write_bytes(s, n, pdMS_TO_TICKS(1000));
}

static void outf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void outf(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    out(line, n < (int)sizeof(line) ? n : sizeof(line) - 1);
}

// The files a ride folder can hold.
static const char *const RIDE_FILES[] = {"obd_raw.log", "trip.csv", "events.csv", "ride.json", "ride.json.tmp"};
#define N_RIDE_FILES (sizeof(RIDE_FILES) / sizeof(RIDE_FILES[0]))

static bool card_ready(void)
{
    sd_status_t st;
    sd_log_status(&st);
    if (!st.mounted) {
        outf("@ERR no card\n");
    }
    return st.mounted;
}

static void cmd_ls(void)
{
    if (!card_ready()) return;
    DIR *d = opendir(ROOT);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
        int n;
        if (sscanf(e->d_name, "ride_%4d", &n) != 1) continue;
        for (size_t i = 0; i < N_RIDE_FILES; i++) {
            char path[96];
            struct stat st;
            snprintf(path, sizeof(path), "%s/ride_%04d/%s", ROOT, n, RIDE_FILES[i]);
            if (stat(path, &st) == 0) {
                outf("@LS ride_%04d/%s %ld\n", n, RIDE_FILES[i], (long)st.st_size);
            }
        }
    }
    if (d) closedir(d);
    struct stat st;
    if (stat(ROOT "/fillups.csv", &st) == 0) {
        outf("@LS fillups.csv %ld\n", (long)st.st_size);
    }
    outf("@END\n");
}

// Only log files: "ride_NNNN/<one of RIDE_FILES>" or the fill-up log,
// nothing else on the card.
static bool valid_name(const char *rel)
{
    if (!strcmp(rel, "fillups.csv")) return true;
    int n, len = 0;
    if (sscanf(rel, "ride_%4d/%n", &n, &len) != 1 || len != 10) return false;
    for (size_t i = 0; i < N_RIDE_FILES; i++) {
        if (!strcmp(rel + len, RIDE_FILES[i])) return true;
    }
    return false;
}

static void cmd_get(const char *rel, long offset)
{
    if (!valid_name(rel)) {
        outf("@ERR bad name\n");
        return;
    }
    if (!card_ready()) return;
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", ROOT, rel);
    FILE *f = fopen(path, "rb");
    struct stat st;
    if (!f || stat(path, &st) != 0 || offset < 0 || offset > st.st_size) {
        if (f) fclose(f);
        outf("@ERR can't open %s\n", rel);
        return;
    }
    long size = st.st_size - offset;
    fseek(f, offset, SEEK_SET);
    outf("@FILE %s %ld\n", rel, size);
    static EXT_RAM_BSS_ATTR uint8_t buf[READ_BUF];
    char line[80];
    uint32_t crc = 0;
    long left = size;
    while (left > 0) {
        size_t want = left < READ_BUF ? left : READ_BUF;
        size_t got = fread(buf, 1, want, f);
        if (got == 0) break;
        crc = esp_rom_crc32_le(crc, buf, got);
        for (size_t i = 0; i < got; i += RAW_LINE) {
            size_t olen = 0, n = got - i < RAW_LINE ? got - i : RAW_LINE;
            mbedtls_base64_encode((unsigned char *)line, sizeof(line) - 1, &olen, buf + i, n);
            line[olen++] = '\n';
            out(line, olen);
        }
        left -= got;
    }
    fclose(f);
    outf("@EOF %08lx\n", (unsigned long)crc);
}

// The Mac's time for the board's clock, and the ride number to carry on from.
static void cmd_sync(long long epoch, int min_next)
{
    bool was_valid = wallclock_valid();
    bool ok = wallclock_set(epoch);
    int next = sd_log_raise_next_ride(min_next);
    if (ok) {
        outf("@OK %d %d\n", was_valid, next);
    } else {
        outf("@ERR clock not set\n");
    }
}

static void handle(char *cmd)
{
    if (strncmp(cmd, "obd2 ", 5) != 0) return;
    cmd += 7;
    // Other tasks' log lines would land in the middle of the data.
    esp_log_level_set("*", ESP_LOG_NONE);
    char name[48];
    long offset = 0;
    long long epoch;
    int next;
    if (!strcmp(cmd, "ls")) {
        cmd_ls();
    } else if (sscanf(cmd, "sync %lld %d", &epoch, &next) == 2) {
        cmd_sync(epoch, next);
    } else if (sscanf(cmd, "get %47s %ld", name, &offset) >= 1) {
        cmd_get(name, offset);
    } else {
        outf("@ERR unknown command\n");
    }
    usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(2000));
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("esp32_s3_touch_lcd_1_85B", ESP_LOG_WARN);   // as in app_main
}

static void export_task(void *arg)
{
    char line[96];
    int len = 0;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) continue;
        if (c == '\r' || c == '\n') {
            line[len] = 0;
            if (len) handle(line);
            len = 0;
        } else if (len < (int)sizeof(line) - 1) {
            line[len++] = (char)c;
        }
    }
}

void log_export_start(void)
{
    usb_serial_jtag_driver_config_t cfg = {.rx_buffer_size = 256, .tx_buffer_size = 2048};
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        ESP_LOGW(TAG, "USB driver install failed: no log download");
        return;
    }
    // The console's USB half now goes through the driver too, instead of
    // writing the FIFO behind the driver's back.
    usb_serial_jtag_vfs_use_driver();
    xTaskCreatePinnedToCore(export_task, "export", 4096, NULL, 1, NULL, 0);
}
