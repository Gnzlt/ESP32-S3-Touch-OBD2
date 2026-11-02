/*
 * The sensor task, 50 Hz: IMU lean and G, and from the OBD values the gear,
 * the trip, the alerts and the coolant history; the battery gauge every 5 s.
 * Saves the trip every minute and when the ECU goes quiet.
 */
#include "qmi8658.h"   /* before math.h: it defines M_PI itself */
#include <math.h>
#include <string.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
#include "app.h"
#include "board_power.h"
#include "obd/board_guard.h"
#include "bike/bike.h"
#include "obd/gear.h"
#include "sensors/beeper.h"
#include "sensors/chip_temp.h"
#include "sensors/motion.h"
#include "sensors/wallclock.h"
#include "sd_log.h"
#include "sensor_task.h"
#include "tpms_service.h"

static const char *TAG = "sensors";

#define PERIOD_MS       20
#define LOGIC_MS        100
#define ALERT_MS        1000
#define HIST_MS         10000
#define BATTERY_MS      5000
#define TRIP_SAVE_MS    60000
#define ROW_MS          200       // trip.csv: 5 rows a second
#define SUMMARY_MS      30000
#ifdef CONFIG_DASH_LOG_DEMO
#define LOG_DEMO 1
#else
#define LOG_DEMO 0
#endif
#define RIDE_END_MS     300000    // 5 minutes without the ECU ends a ride (a stall doesn't)
#define ROW_GRACE_MS    10000
#define FUEL_HOLD_MS    2000      // a lost load/rpm sample keeps the last fuel estimate this long     // trip.csv rows continue this long after the ECU goes quiet
// The IMU's z axis points out of the screen, toward the rider; forward is
// the opposite way. Flip this if lean reads left when the board tilts right.
#define MOTION_FLIP     (-1.0f)

static qmi8658_dev_t s_imu;
static bool s_imu_ok;
static motion_est_t s_motion;
static trip_t s_trip;        // the real ride, saved in NVS
static trip_t s_demo_trip;   // demo mode's own, never saved, so a demo can't pollute the real trip
static alerts_t s_alerts;
static gear_t s_gear;
static trip_t s_ride;        // this ride's totals, for its summary on the card
static fill_t s_fill;        // fuel since the last fill-up, saved in NVS
static fill_t s_demo_fill;   // demo's own, like s_demo_trip
static float s_fuel_lph = -1.0f;    // raw estimate this tick, < 0 = unknown
static float s_fuel_held;           // the last good estimate...
static int64_t s_fuel_at = -FUEL_HOLD_MS - 1;   // ...and when it was made
static bool s_ride_open;
static int64_t s_ride_start, s_ride_last_live;
static uint32_t s_ride_alerts;
static char s_ride_codes[96];
static char s_ride_uid[9];
static obd_phase_t s_ride_phase;
static uint32_t s_tyre_seq[TPMS_TYRES];   // the tyre readings already in events.csv

static int s_ride_batt_start = -1;  // the board battery % when the ride opened, -1 with no cell
static double s_ride_ma_ms;         // the cell's current over the ride, mA x ms
static int64_t s_ride_cur_ms;       // ...and the time it covers
static float s_ride_max_chip = NAN, s_ride_max_cell = NAN;   // the hottest the board got this ride
static board_guard_t s_guard;       // the board's own battery and heat (obd/board_guard.h)
static int s_saver_mine = -1;       // the saver value the guard just set, until apply_settings sees it

static const char *ALERT_NAMES[] = {"", "coolant", "volt_high", "volt_low", "dtc", "board_batt", "board_hot", "tyre_loss",
                                    "tyre_low", "tyre_alarm", "tyre_hot", "tyre_high", "tyre_cold", "tyre_silent",
                                    "tyre_batt"};
_Static_assert(ALERT_TYRE_BATT - ALERT_TYRE_LOSS == TPMS_A_BATTERY - TPMS_A_LOSS, "tyre alerts in tpms order");
_Static_assert(sizeof(ALERT_NAMES) / sizeof(ALERT_NAMES[0]) == ALERT_COUNT, "a name per alert");
// events.csv's names for the OBD phases (obd_phase_t order).
static const char *PHASE_NAMES[] = {"no_ble", "scan", "connect", "init", "ecu_off", "live"};
_Static_assert(sizeof(PHASE_NAMES) / sizeof(PHASE_NAMES[0]) == PHASE_LIVE + 1, "a name per phase");

static void fmt_field(char **p, size_t *left, bool has, float v, const char *fmt)
{
    int n = has ? snprintf(*p, *left, fmt, v) : 0;
    n += snprintf(*p + n, *left - n, ",");
    *p += n;
    *left -= n;
}

static motion_t motion_snapshot(void)
{
    return (motion_t){s_imu_ok, s_motion.calibrated, s_imu_ok && s_motion.valid, s_motion.lean,
                      s_motion.lat_g, s_motion.lon_g, s_motion.pitch};
}

// One trip.csv row, matching SD_TRIP_HEADER.
static void log_row(const dash_t *dash, int64_t now)
{
    const obd_state_t *o = &dash->obd;
    char row[256], *p = row;
    size_t left = sizeof(row);
    float v;
    int n = snprintf(p, left, "%.1f,", (now - s_ride_start) / 1000.0);
    p += n; left -= n;
    fmt_field(&p, &left, obd_value(o, PID_RPM, now, &v), v, "%.0f");
    fmt_field(&p, &left, obd_value(o, PID_SPEED, now, &v), v, "%.0f");
    fmt_field(&p, &left, dash->gear > 0, (float)dash->gear, "%.0f");
    fmt_field(&p, &left, obd_value(o, PID_THROTTLE, now, &v), v, "%.1f");
    fmt_field(&p, &left, obd_value(o, PID_LOAD, now, &v), v, "%.1f");
    fmt_field(&p, &left, obd_value(o, PID_MAP, now, &v), v, "%.0f");
    fmt_field(&p, &left, obd_value(o, PID_COOLANT, now, &v), v, "%.0f");
    fmt_field(&p, &left, obd_value(o, PID_IAT, now, &v), v, "%.0f");
    fmt_field(&p, &left, obd_value(o, PID_ADVANCE, now, &v), v, "%.1f");
    fmt_field(&p, &left, obd_value(o, PID_STFT1, now, &v), v, "%.1f");
    // The loop state's raw byte: 1 open loop (cold), 2 closed loop, 4 open loop
    // (load or decel fuel cut), 8/16 open loop on a fault.
    fmt_field(&p, &left, obd_value(o, PID_FUEL_STATUS, now, &v), (float)o->raw[PID_FUEL_STATUS][0], "%.0f");
    fmt_field(&p, &left, s_fuel_lph >= 0, s_fuel_lph, "%.2f");
    fmt_field(&p, &left, true, (float)s_ride.fuel_raw_ml, "%.0f");
    fmt_field(&p, &left, obd_value(o, PID_VOLTAGE, now, &v), v, "%.2f");
    bool riding = s_imu_ok && s_motion.valid;   // standing still, lean and G mean nothing
    fmt_field(&p, &left, riding, s_motion.lean, "%.1f");
    fmt_field(&p, &left, riding, s_motion.lat_g, "%.2f");
    fmt_field(&p, &left, riding, s_motion.lon_g, "%.2f");
    fmt_field(&p, &left, true, (float)(s_ride.distance_m / 1000.0), "%.2f");
    // The board's own LiPo, empty with no cell.
    bool cell = dash->battery_ok && dash->battery.present;
    fmt_field(&p, &left, cell, (float)dash->battery.soc_percent, "%.0f");
    n = cell ? snprintf(p, left, "%u,", dash->battery.voltage_mv) : snprintf(p, left, ",");
    p += n; left -= n;
    // The tyres, empty when unpaired or their reading is stale.
    const tpms_view_t *tp = &dash->tpms;
    for (int t = 0; t < TPMS_TYRES; t++) {
        const tpms_tyre_t *y = &tp->tyre[t];
        int64_t age = tpms_tyre_age(y, now);
        bool ok = tp->pair[t].format && age >= 0 && age < TPMS_STALE_MS;
        fmt_field(&p, &left, ok && !isnan(y->kpa), y->kpa, "%.0f");
        fmt_field(&p, &left, ok && !isnan(y->temp_c), y->temp_c, "%.0f");
    }
    // The board's temperatures: the ESP32-S3's die, and the gauge beside the cell.
    fmt_field(&p, &left, !isnan(dash->chip_c), dash->chip_c, "%.1f");
    fmt_field(&p, &left, cell, dash->battery.temp_c, "%.1f");
    // The cell's current (+ charging, - discharging), to see where the charge goes.
    fmt_field(&p, &left, cell, (float)dash->battery.current_ma, "%.0f");
    if (p > row && p[-1] == ',') p[-1] = 0;   // no comma after the last column
    sd_log_trip_row(row);
}

// One events.csv row for the open ride.
static void log_event(int64_t now, const char *type, const char *detail)
{
    if (s_ride_open) sd_log_event((now - s_ride_start) / 1000.0, type, detail);
}

// Codes seen during the ride, not just the current ones: each new one is
// added to s_ride_codes and logged as an event.
// Each tyre reading the service flagged: "front 194 21 -86" (kPa, °C, dBm).
static void log_tyres(const tpms_view_t *tp, int64_t now)
{
    for (int t = 0; t < TPMS_TYRES; t++) {
        if (tp->reading_seq[t] == s_tyre_seq[t]) continue;
        s_tyre_seq[t] = tp->reading_seq[t];
        char detail[40], temp[8] = "";
        if (!isnan(tp->reading_c[t])) snprintf(temp, sizeof(temp), "%.0f", tp->reading_c[t]);
        snprintf(detail, sizeof(detail), "%s %.0f %s %d", TPMS_TYRE_NAMES[t], tp->reading_kpa[t], temp,
                 tp->reading_rssi[t]);
        log_event(now, "tyre", detail);
    }
}

static void note_codes(const obd_state_t *o, int64_t now)
{
    for (int i = 0; i < o->n_dtcs + o->n_pending; i++) {
        const char *c = i < o->n_dtcs ? o->dtcs[i] : o->pending[i - o->n_dtcs];
        if (!strstr(s_ride_codes, c) && strlen(s_ride_codes) + 8 < sizeof(s_ride_codes)) {
            if (s_ride_codes[0]) strcat(s_ride_codes, " ");
            strcat(s_ride_codes, c);
            log_event(now, "code", c);
        }
    }
}

// The ride's ride.json (obd/ride_summary.h).
static void log_summary(const dash_t *dash, int64_t now, bool final)
{
    const obd_state_t *o = &dash->obd;
    const trip_t *t = &s_ride;
    static EXT_RAM_BSS_ATTR ride_report_t r;   // the sensor task's stack is tight
    ride_report_init(&r);
    sd_status_t sd;
    sd_log_status(&sd);
    r.ride = sd.logging ? sd.ride : 0;
    snprintf(r.uid, sizeof(r.uid), "%s", s_ride_uid);
    r.in_progress = !final;
    if (wallclock_valid()) {
        // From the clock now, so a clock set mid-ride still dates the ride.
        time_t start = time(NULL) - (time_t)((now - s_ride_start) / 1000);
        struct tm tm;
        gmtime_r(&start, &tm);
        strftime(r.start, sizeof(r.start), "%Y-%m-%dT%H:%M:%SZ", &tm);
    }
    snprintf(r.firmware, sizeof(r.firmware), "%s", esp_app_get_description()->version);
    snprintf(r.bike, sizeof(r.bike), "%s", BIKE_NAME);
    // To the last moment the ECU answered: the 5 minutes waiting for it to
    // come back aren't riding.
    r.duration_s = (uint32_t)((s_ride_last_live - s_ride_start) / 1000);
    r.engine_s = t->engine_s;
    r.distance_km = (float)(t->distance_m / 1000.0);
    r.avg_kmh = trip_avg_kmh(t);
    r.max_kmh = t->max_kmh;
    r.max_rpm = t->max_rpm;
    if (t->max_coolant > 0) r.max_coolant_c = t->max_coolant;
    if (t->min_volts > 0) r.min_volts = t->min_volts;
    if (s_imu_ok) {
        r.max_lean_left_deg = t->max_lean_left;
        r.max_lean_right_deg = t->max_lean_right;
        r.max_g = t->max_g;
        if (s_motion.bias_s > 0) memcpy(r.gyro_bias_dps, s_motion.gyro_bias, sizeof(r.gyro_bias_dps));
    }
    r.alerts = s_ride_alerts;
    snprintf(r.codes, sizeof(r.codes), "%s", s_ride_codes);
    // Estimated fuel, raw (uncalibrated) as the model gives it, with what a
    // later calibration needs: k, the model version and the coverage.
    r.fuel_raw_l = (float)(t->fuel_raw_ml / 1000.0);
    float l100 = trip_raw_l100(t);
    if (l100 >= 0) r.fuel_raw_l100 = l100;
    r.fuel_k = s_fill.k;
    r.fuel_model = FUEL_MODEL_VERSION;
    r.fuel_coverage_pct = fuel_coverage_pct(t->fuel_covered_ms, t->fuel_missing_ms);
    r.since_fill_km = (float)s_fill.km;
    r.since_fill_raw_l = (float)(s_fill.raw_ml / 1000.0);
    r.fills = (int)s_fill.fills;
    const battery_info_t *b = &dash->battery;
    r.batt_cell = dash->battery_ok && b->present;
    if (r.batt_cell) {
        r.batt_start_pct = s_ride_batt_start;
        r.batt_end_pct = b->soc_percent;
        r.batt_end_mv = b->voltage_mv;
        r.batt_charging = b->charging;
        if (s_ride_cur_ms > 0) r.batt_avg_ma = (float)(s_ride_ma_ms / s_ride_cur_ms);
        r.batt_full_mah = b->full_mah;
        r.batt_remaining_mah = b->remaining_mah;
    }
    r.max_chip_c = s_ride_max_chip;
    r.max_cell_c = s_ride_max_cell;
    const tpms_view_t *tp = &dash->tpms;
    for (int i = 0; i < TPMS_TYRES && i < 2; i++) {
        r.tyre[i] = (ride_tyre_t){tp->ride_min_kpa[i], tp->ride_max_kpa[i], tp->ride_max_c[i],
                                  tp->demo || tp->pair[i].format != TPMS_F_NONE, tp->ride_heard[i],
                                  tp->ride_min_rssi[i]};
    }
    snprintf(r.ble_name, sizeof(r.ble_name), "%s", o->ble_name);
    snprintf(r.adapter, sizeof(r.adapter), "%s", o->adapter);
    snprintf(r.protocol, sizeof(r.protocol), "%s", o->protocol);
    snprintf(r.ecu, sizeof(r.ecu), "%s", o->ecu);
    // Mode 09: VIN, ECU software (calibration IDs) and their checksums (CVN).
    snprintf(r.vin, sizeof(r.vin), "%s", o->vin);
    for (int i = 0; i < 2; i++) {
        snprintf(r.cal_id[i], sizeof(r.cal_id[i]), "%s", o->cal_id[i]);
        snprintf(r.cvn[i], sizeof(r.cvn[i]), "%s", o->cvn[i]);
    }
    r.timeouts = o->timeouts;
    r.reinits = o->reinits;
    r.link_drops = o->link_drops;
    r.ecu_drops = o->ecu_drops;
    static EXT_RAM_BSS_ATTR char text[SD_SUMMARY_MAX];
    if (ride_report_json(&r, text, sizeof(text))) {
        sd_log_summary(text);
    } else {
        ESP_LOGW(TAG, "ride.json doesn't fit in %d bytes", SD_SUMMARY_MAX);
    }
}

static bool imu_init(void)
{
    if (qmi8658_init(&s_imu, bsp_i2c_get_handle(), QMI8658_ADDRESS_HIGH) != ESP_OK) {
        return false;
    }
    qmi8658_set_accel_range(&s_imu, QMI8658_ACCEL_RANGE_4G);
    qmi8658_set_accel_odr(&s_imu, QMI8658_ACCEL_ODR_125HZ);
    qmi8658_set_gyro_range(&s_imu, QMI8658_GYRO_RANGE_512DPS);
    qmi8658_set_gyro_odr(&s_imu, QMI8658_GYRO_ODR_125HZ);
    qmi8658_set_accel_unit_mps2(&s_imu, true);
    qmi8658_set_gyro_unit_dps(&s_imu, true);
    qmi8658_enable_sensors(&s_imu, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);
    return true;
}

// The backlight is the UI's (ui.c): it knows about the parked and saver screens.
static void apply_settings(void)
{
    static int last_saver = -1;
    settings_t *st = app_settings();
    s_alerts.enabled = st->alerts;
    if (st->saver != last_saver) {
        bool mine = st->saver == s_saver_mine;
        if (last_saver >= 0) {
            log_event(app_now_ms(), "display", st->saver ? (mine ? "saver on (hot)" : "saver on")
                                                         : (mine ? "saver off (cooled)" : "saver off"));
        }
        if (!mine) guard_saver_changed(&s_guard);   // the user's, or the battery alert's: not the heat's to undo
        s_saver_mine = -1;
        last_saver = st->saver;
    }
}

static void beep_for(alert_t a)
{
    if (!app_settings()->beeps) return;
    switch (a) {
    case ALERT_COOLANT: beeper_play(BEEP_URGENT); break;
    case ALERT_VOLT_HIGH:
    case ALERT_VOLT_LOW:
    case ALERT_BOARD_BATT: beeper_play(BEEP_WARN); break;
    case ALERT_BOARD_HOT: beeper_play(s_alerts.hot_critical ? BEEP_URGENT : BEEP_WARN); break;
    case ALERT_DTC: beeper_play(BEEP_INFO); break;
    case ALERT_TYRE_LOSS:
    case ALERT_TYRE_LOW:
    case ALERT_TYRE_ALARM: beeper_play(BEEP_URGENT); break;
    case ALERT_TYRE_HOT:
    case ALERT_TYRE_HIGH: beeper_play(BEEP_WARN); break;
    case ALERT_TYRE_COLD:
    case ALERT_TYRE_SILENT:
    case ALERT_TYRE_BATT: beeper_play(BEEP_INFO); break;
    default: break;
    }
}

// A fill-up with the litres pumped: one calibration point (litres against
// the raw estimate since the previous fill-up) in fillups.csv, then the
// counters start again. Demo rows say so, to be left out of a calibration.
static void fill_up(fill_t *f, float litres, bool demo, int64_t now)
{
    sd_status_t sd;
    sd_log_status(&sd);
    char ride[12];
    if (demo) {
        snprintf(ride, sizeof(ride), "demo");
    } else {
        snprintf(ride, sizeof(ride), "%d", sd.logging ? sd.ride : 0);
    }
    char when[24] = "";
    if (wallclock_valid()) {
        time_t t = time(NULL);
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(when, sizeof(when), "%Y-%m-%dT%H:%M:%SZ", &tm);
    }
    char row[160];
    snprintf(row, sizeof(row), "%lu,%s,%.0f,%.2f,%.3f,%.1f,%.3f,%d,%d,%s", (unsigned long)(f->fills + 1), ride,
             now / 1000.0, f->km, f->raw_ml / 1000.0, litres, f->k, FUEL_MODEL_VERSION,
             fuel_coverage_pct(f->covered_ms, f->missing_ms), when);
    sd_log_fillup(row);
    char litres_text[16];
    snprintf(litres_text, sizeof(litres_text), "%.1f L", litres);
    log_event(now, "fillup", litres_text);
    char note[96];
    snprintf(note, sizeof(note), "fill-up %.1f L after %.1f km, %.2f L raw estimated", litres, f->km,
             f->raw_ml / 1000.0);
    sd_log_note(note, now);
    ESP_LOGI(TAG, "%s", note);
    fill_mark(f);
    if (!demo) nvs_set_blob_("fill", f, sizeof(*f));
}

// The guard switches the board off (obd/board_guard.h, board_power.h): the
// open ride is closed with a last event, the trip saved, the card unmounted,
// a message shown for 3 s, then deep sleep. Doesn't return.
static void switch_off(guard_action_t why, bool demo, int64_t now)
{
    const char *what = board_power_reason(why);
    ESP_LOGW(TAG, "board %s: switching off", what);
    app_lock();
    dash_t *dash = app_dash();
    dash->switching_off = (uint8_t)why;
    if (s_ride_open) {
        char detail[24];
        snprintf(detail, sizeof(detail), "off %s", what);
        log_event(now, "board", detail);
        log_summary(dash, now, true);
        s_ride_open = false;
    }
    app_unlock();
    if (!demo) {
        nvs_set_blob_("trip", &s_trip, sizeof(s_trip));
        nvs_set_blob_("fill", &s_fill, sizeof(s_fill));
    }
    sd_log_shutdown();
    for (int i = 0; i < 50 && !sd_log_is_shut_down(); i++) vTaskDelay(pdMS_TO_TICKS(100));
    vTaskDelay(pdMS_TO_TICKS(3000));   // the message (ui.c)
    board_power_sleep(why);
}

static void set_saver(int on)
{
    s_saver_mine = on;
    app_settings()->saver = (uint8_t)on;
    app_settings_changed();
}

static void sensor_task(void *arg)
{
    s_imu_ok = imu_init();
    ESP_LOGI(TAG, "IMU %s", s_imu_ok ? "ok" : "not answering");
    motion_init(&s_motion, MOTION_FLIP);
    float up[3];
    if (nvs_get_blob_("up", up, sizeof(up))) {
        motion_set_up(&s_motion, up, true);
    }
    if (!nvs_get_blob_("trip", &s_trip, sizeof(s_trip)) || s_trip.version != TRIP_VERSION) {
        trip_reset(&s_trip);
    }
    if (!nvs_get_blob_("fill", &s_fill, sizeof(s_fill)) || s_fill.version != FILL_VERSION) {
        fill_reset(&s_fill);
    }
    alerts_init(&s_alerts);
    guard_init(&s_guard);
    if (beeper_init() != ESP_OK) {
        ESP_LOGW(TAG, "no speaker");
    }
    bool battery_ok = battery_init() == ESP_OK;
    chip_temp_init();
    float chip_c = NAN;
    bool temps_fresh = false;   // a new temperature reading not yet checked for an alert
    apply_settings();
    tpms_service_init();

    TickType_t wake = xTaskGetTickCount();
    int64_t last_row = 0, last_summary = 0;
    int64_t last_logic = 0, last_alert = 0, last_hist = 0, last_battery = -BATTERY_MS, last_save = app_now_ms();
    obd_phase_t last_phase = PHASE_SCAN;
    float kmh = -1;
    float accel[3] = {0, 0, 0};
    battery_info_t bat = {0};
    bool demo = false;
    trip_t *trip = &s_trip;
    fill_t *fill = &s_fill;

    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(PERIOD_MS));
        int64_t now = app_now_ms();

        uint32_t req = app_take_requests(REQ_RESET_TRIP | REQ_ZERO_LEAN | REQ_SETTINGS_CHANGED | REQ_RESET_LEAN_MAX
                                         | REQ_FILL_UP);
        if (req & REQ_FILL_UP) {
            fill_up(fill, app_take_fill_litres(), demo, now);
        }
        if (req & REQ_SETTINGS_CHANGED) apply_settings();
        if (req & REQ_RESET_TRIP) {
            trip_reset(trip);
            s_alerts.fired = 0;
            if (!demo) nvs_set_blob_("trip", &s_trip, sizeof(s_trip));
        }
        if (req & REQ_RESET_LEAN_MAX) {
            trip->max_lean_left = trip->max_lean_right = trip->max_g = 0;
        }
        if ((req & REQ_ZERO_LEAN) && s_imu_ok) {
            motion_zero(&s_motion, accel);
            nvs_set_blob_("up", s_motion.up, sizeof(s_motion.up));
            ESP_LOGI(TAG, "lean zeroed");
        }

        if (s_imu_ok) {
            qmi8658_data_t d;
            if (qmi8658_read_sensor_data(&s_imu, &d) == ESP_OK) {
                accel[0] = d.accelX; accel[1] = d.accelY; accel[2] = d.accelZ;
                float gyro[3] = {d.gyroX, d.gyroY, d.gyroZ};
                motion_update(&s_motion, accel, gyro, PERIOD_MS / 1000.0f, kmh);
            }
        }

        if (now - last_battery >= BATTERY_MS) {
            last_battery = now;
            if (battery_ok) battery_ok = battery_read(&bat) == ESP_OK;
            if (!chip_temp_read(&chip_c)) chip_c = NAN;
            temps_fresh = true;
            // The board looking after itself: heat turns battery saver on,
            // too hot or an empty cell switches it off.
            bool cell = battery_ok && bat.present;
            guard_in_t gi = {
                .has_chip = !isnan(chip_c), .chip_c = chip_c, .has_cell = cell, .cell_c = bat.temp_c,
                .battery = cell, .mv = bat.voltage_mv, .charging = bat.charging,
                .low = cell && bat.soc_percent <= BOARD_BATT_LOW_PCT, .saver = app_settings()->saver,
            };
            guard_action_t ga = guard_update(&s_guard, &gi, now);
            if (ga == GUARD_SAVER_ON || ga == GUARD_SAVER_OFF) {
                ESP_LOGI(TAG, "battery saver %s: board %s", ga == GUARD_SAVER_ON ? "on" : "off",
                         ga == GUARD_SAVER_ON ? "warm" : "cooled down");
                set_saver(ga == GUARD_SAVER_ON);
            } else if (ga == GUARD_OFF_EMPTY || ga == GUARD_OFF_HOT) {
                switch_off(ga, demo, now);
            }
        }

        tpms_service_poll(now, demo);

        if (now - last_logic < LOGIC_MS) {
            app_lock();
            dash_t *dash = app_dash();
            dash->motion = motion_snapshot();
            app_unlock();
            continue;
        }
        int dt = (int)(now - last_logic);
        last_logic = now;

        app_lock();
        dash_t *dash = app_dash();
        const obd_state_t *o = &dash->obd;
        float rpm = 0, speed = 0, cool = 0, volts = 0;
        bool has_rpm = obd_value(o, PID_RPM, now, &rpm);
        bool has_speed = obd_value(o, PID_SPEED, now, &speed);
        bool has_cool = obd_value(o, PID_COOLANT, now, &cool);
        bool has_volts = obd_value(o, PID_VOLTAGE, now, &volts);
        int dtc_total = o->n_dtcs + o->n_pending;
        bool mil = o->mil;
        obd_phase_t phase = o->phase;
        kmh = has_speed ? speed : -1;
        if (dash->demo != demo) {
            // Switching modes: fresh alerts, and demo gets a fresh trip of its own.
            demo = dash->demo;
            alerts_init(&s_alerts);
            s_alerts.enabled = app_settings()->alerts;
            if (demo) {
                trip_reset(&s_demo_trip);
                fill_reset(&s_demo_fill);
            }
            trip = demo ? &s_demo_trip : &s_trip;
            fill = demo ? &s_demo_fill : &s_fill;
        }

        dash->gear = gear_update(&s_gear, has_rpm && has_speed, rpm, speed);
        trip_update(trip, dt, has_speed, speed, has_rpm, rpm, has_cool, cool, has_volts, volts);

        // Estimated fuel (obd/fuel.h), raw: logs keep it uncalibrated, the
        // screen multiplies by fill->k.
        float load = 0, stft = 0, fsv;
        fuel_in_t fin = {
            .has_rpm = has_rpm, .rpm = rpm,
            .has_load = obd_value(o, PID_LOAD, now, &load), .load_pct = load,
            .has_stft = obd_value(o, PID_STFT1, now, &stft), .stft_pct = stft,
            .status = obd_value(o, PID_FUEL_STATUS, now, &fsv) ? o->raw[PID_FUEL_STATUS][0] : 0,
        };
        bool running = has_rpm && rpm >= 300.0f;
        s_fuel_lph = fuel_rate_lph(&fin);
        if (s_fuel_lph >= 0) {
            s_fuel_held = s_fuel_lph;
            s_fuel_at = now;
        } else if (running && now - s_fuel_at <= FUEL_HOLD_MS) {
            s_fuel_lph = s_fuel_held;
        }
        trip_add_fuel(trip, dt, running, s_fuel_lph);
        fill_add(fill, dt, running, s_fuel_lph, has_speed, speed);
        dash->fill = *fill;
        dash->fuel_lph = s_fuel_lph;

        // Lean and G only while riding; NAN between rides breaks the one-second hold.
        bool moving = s_imu_ok && s_motion.valid;
        float lean_now = moving ? s_motion.lean : NAN;
        float g_now = moving ? sqrtf(s_motion.lat_g * s_motion.lat_g + s_motion.lon_g * s_motion.lon_g) : NAN;
        trip_update_motion(trip, dt, lean_now, g_now);
        alert_t fired = ALERT_NONE;
        if (now - last_alert >= ALERT_MS) {
            last_alert = now;
            fired = alerts_evaluate(&s_alerts, has_cool, cool, has_volts, volts, has_rpm, rpm, dtc_total, mil, now);
            // Tyres next: one not reported this second stays armed for the next.
            int tyre = -1;
            if (fired == ALERT_NONE && s_alerts.enabled) {
                bool riding = (has_speed && speed > 0.0f) || (s_imu_ok && s_motion.valid);
                tpms_alert_t ta = tpms_service_alert(now, riding, &tyre);
                if (ta != TPMS_A_NONE) {
                    fired = (alert_t)(ALERT_TYRE_LOSS + (ta - TPMS_A_LOSS));
                    s_alerts.fired++;
                    dash->alert_tyre = tyre;
                }
            }
            if (fired == ALERT_NONE) {
                fired = alerts_battery(&s_alerts, battery_ok && bat.present, bat.soc_percent, bat.charging, now);
            }
            // The board overheating, once per reading; one that lost to another
            // alert this second is checked the next.
            if (fired == ALERT_NONE && temps_fresh) {
                temps_fresh = false;
                bool cell = battery_ok && bat.present;
                fired = alerts_board_temp(&s_alerts, !isnan(chip_c), chip_c, cell, bat.temp_c, cell && bat.overtemp, now);
                if (fired == ALERT_BOARD_HOT) {
                    dash->hot_source = s_alerts.hot_source;
                    dash->hot_critical = s_alerts.hot_critical;
                    dash->hot_c = s_alerts.hot_c;
                }
            }
            if (fired != ALERT_NONE) {
                dash->alert = fired;
                dash->alert_seq++;
            }
        }
        trip->alerts = s_alerts.fired;
        if (now - last_hist >= HIST_MS && has_cool) {
            last_hist = now;
            if (dash->coolant_hist_n == COOLANT_HIST) {
                memmove(dash->coolant_hist, dash->coolant_hist + 1, sizeof(float) * (COOLANT_HIST - 1));
                dash->coolant_hist_n--;
            }
            dash->coolant_hist[dash->coolant_hist_n++] = cool;
            dash->coolant_hist_seq++;
        }
        dash->trip = *trip;

        dash->battery = bat;
        dash->battery_ok = battery_ok;
        dash->chip_c = chip_c;
        tpms_service_publish(&dash->tpms, now);

        // Ride logs on the card: a ride opens when the ECU answers and closes
        // after 5 minutes without it, so a stall or a fuel stop stays one ride.
        bool may_log = app_settings()->sd_log && (!demo || LOG_DEMO);
        bool live = phase == PHASE_LIVE;
        if (may_log && live) {
            s_ride_last_live = now;
            if (!s_ride_open) {
                s_ride_open = true;
                s_ride_start = now;
                s_ride_alerts = 0;
                s_ride_codes[0] = 0;
                s_ride_phase = phase;
                snprintf(s_ride_uid, sizeof(s_ride_uid), "%08lx", (unsigned long)esp_random());
                s_ride_batt_start = battery_ok && bat.present ? bat.soc_percent : -1;
                s_ride_ma_ms = 0;
                s_ride_cur_ms = 0;
                s_ride_max_chip = s_ride_max_cell = NAN;
                trip_reset(&s_ride);
                tpms_service_new_ride();
                sd_log_ride_begin(s_ride_start);
            }
        }
        if (s_ride_open) {
            trip_update(&s_ride, dt, has_speed, speed, has_rpm, rpm, has_cool, cool, has_volts, volts);
            if (!isnan(chip_c) && (isnan(s_ride_max_chip) || chip_c > s_ride_max_chip)) s_ride_max_chip = chip_c;
            if (battery_ok && bat.present && (isnan(s_ride_max_cell) || bat.temp_c > s_ride_max_cell)) {
                s_ride_max_cell = bat.temp_c;
            }
            trip_add_fuel(&s_ride, dt, running, s_fuel_lph);
            if (battery_ok && bat.present) {
                s_ride_ma_ms += (double)bat.current_ma * dt;
                s_ride_cur_ms += dt;
            }
            trip_update_motion(&s_ride, dt, lean_now, g_now);
            if (fired != ALERT_NONE) {
                s_ride_alerts++;
                char detail[24];
                bool tyre = fired >= ALERT_TYRE_LOSS && dash->alert_tyre >= 0 && dash->alert_tyre < TPMS_TYRES;
                snprintf(detail, sizeof(detail), "%s%s%s", ALERT_NAMES[fired < ALERT_COUNT ? fired : 0],
                         tyre ? " " : "", tyre ? TPMS_TYRE_NAMES[dash->alert_tyre] : "");
                log_event(now, "alert", detail);
            }
            if (phase != s_ride_phase) {
                s_ride_phase = phase;
                log_event(now, "obd", PHASE_NAMES[phase <= PHASE_LIVE ? phase : PHASE_SCAN]);
            }
            note_codes(o, now);
            log_tyres(&dash->tpms, now);
            // Rows stop 10 s after the ECU went quiet (the ride stays open
            // 5 minutes for a stall or a fuel stop) and resume when it's back.
            bool ecu_recent = now - s_ride_last_live <= ROW_GRACE_MS;
            if (ecu_recent && now - last_row >= ROW_MS) {
                last_row = now;
                log_row(dash, now);
            }
            bool end = !may_log || now - s_ride_last_live > RIDE_END_MS;
            if (end || now - last_summary >= SUMMARY_MS) {
                last_summary = now;
                log_summary(dash, now, end);
            }
            if (end) {
                s_ride_open = false;
                sd_log_ride_end();
            }
        }
        dash->motion = motion_snapshot();
        app_unlock();

        if (fired != ALERT_NONE) {
            ESP_LOGW(TAG, "alert %d", fired);
            beep_for(fired);
        }
        // The board battery at 20 % (or 10 %) turns battery saver on, once per crossing.
        if (fired == ALERT_BOARD_BATT && !app_settings()->saver) {
            app_settings()->saver = 1;
            app_settings_changed();
        }
        bool went_quiet = last_phase == PHASE_LIVE && phase != PHASE_LIVE;
        last_phase = phase;
        if (!demo && (now - last_save >= TRIP_SAVE_MS || went_quiet)) {
            last_save = now;
            nvs_set_blob_("trip", &s_trip, sizeof(s_trip));
            nvs_set_blob_("fill", &s_fill, sizeof(s_fill));
        }
    }
}

void sensor_task_start(void)
{
    xTaskCreatePinnedToCore(sensor_task, "sensors", 6144, NULL, 4, NULL, 0);
}
