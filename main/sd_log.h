/*
 * Ride logs on the microSD card, one folder per ride:
 *
 *   /sdcard/obd2/ride_0007/trip.csv      decoded values, 5 rows a second (SD_TRIP_HEADER)
 *   /sdcard/obd2/ride_0007/events.csv    alerts, new fault codes, OBD phase changes, fill-ups
 *   /sdcard/obd2/ride_0007/obd_raw.log   every ELM327 command and reply, as sent and received
 *   /sdcard/obd2/ride_0007/ride.json     the ride's totals and details (obd/ride_summary.h), rewritten every 30 s
 *   /sdcard/obd2/fillups.csv             every fill-up marked on the Fuel page
 *   /sdcard/obd2/tpms.cfg                the tyre sensors and their alert limits
 *
 * The OBD and sensor tasks only copy lines into buffers; a low-priority
 * writer task does the file work, flushing and syncing every 5 s so a power
 * cut (ignition off) loses at most a few seconds. The card is never formatted.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "obd/ride_summary.h"

typedef struct {
    bool mounted;
    bool logging;            // a ride folder is open
    int ride;                // its number
    uint32_t free_mb, total_mb;
    uint32_t written_kb;     // this ride
    char error[40];
    uint32_t formats;        // bumps after each successful format
    bool formatting;
} sd_status_t;

// Mount the card (in the writer task, so boot doesn't wait) and start writing.
void sd_log_start(void);
void sd_log_status(sd_status_t *out);

// From the sensor task: open a new ride folder / close the current one.
// One fill-up row for /obd2/fillups.csv (header SD_FILL_HEADER), written
// whether or not a ride is open; held until the card is mounted.
extern const char *SD_FILL_HEADER;
void sd_log_fillup(const char *row);
// start_ms: the ride's t_s 0 (ms since boot), named in obd_raw.log's header.
void sd_log_ride_begin(int64_t start_ms);
void sd_log_ride_end(void);
// The board is switching itself off (board_power.h): what is buffered goes to
// the card, the open ride closes and the card is unmounted. Nothing is written
// after it. sd_log_is_shut_down() says when it's done.
void sd_log_shutdown(void);
bool sd_log_is_shut_down(void);

// From the OBD task: a command sent ('>') or a reply received ('<'), raw bytes.
void sd_log_raw(char dir, const uint8_t *data, int len, int64_t now_ms);
// A comment line in the raw log ("# link up").
void sd_log_note(const char *text, int64_t now_ms);

// From the sensor task: one trip.csv row (no newline), one events.csv row
// (type and detail without commas), and the ride.json text.
#define SD_SUMMARY_MAX 2048
void sd_log_trip_row(const char *row);
void sd_log_event(double t_s, const char *type, const char *detail);
void sd_log_summary(const char *text);

// Ride numbers never go below NVS "ride_next": raise it to at least
// at_least (never lower) and return it. tools/pull_logs.py sends one more than
// the highest ride it already has, so a fresh card can't reuse a number.
int sd_log_raise_next_ride(int at_least);

// Erase the card and make a fresh FAT32 file system (the Status page's
// "Format SD card", after a confirm). Runs in the writer task; the result
// shows in sd_status_t (formatted / error).
void sd_log_format(void);

// The Rides page: the newest SD_RIDES_MAX rides' ride.json, read off the card
// by the writer task. refresh() asks for a fresh read; sd_log_rides() copies
// what the last read found, newest first, and returns how many (-1 before the
// first read finishes). *seq bumps after every read.
#define SD_RIDES_MAX 60
void sd_log_rides_refresh(void);
int sd_log_rides(ride_summary_t *out, int max, uint32_t *seq);

// The tyre sensors' configuration file, /obd2/tpms.cfg (tpms/tpms_cfgfile.h).
// put() hands the writer task new text, written as soon as the card is
// mounted; the latest text wins. get() copies the text read at the last mount
// (or format), "" when the card has no such file, only when there has been a
// read since *seq: it returns true and updates *seq (start it at 0).
#define SD_TPMS_CFG_MAX 2048
void sd_log_tpms_cfg_put(const char *text);
bool sd_log_tpms_cfg_get(char *out, size_t n, uint32_t *seq);

// The CSV headers that match the rows the sensor task writes.
extern const char *SD_TRIP_HEADER;
extern const char *SD_EVENT_HEADER;
