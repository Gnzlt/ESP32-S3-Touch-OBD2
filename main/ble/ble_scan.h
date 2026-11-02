/*
 * The one BLE scan. NimBLE runs a single discovery procedure at a time, so
 * the adapter search (obd_link.c) and the tyre sensors (tpms_service.c) share
 * this one, and every advertisement it hears goes to both. Each client says
 * what it wants and the scan runs in the mode that serves them:
 *   - adapter search: active (adapters often put their name in the scan
 *     response), 40 ms of every 50;
 *   - tyre sensors only: passive, 50 ms of every 100, or 30 ms of every 100
 *     while the adapter is connected, so the OBD link keeps most of the radio.
 * ble_gap_connect is refused while a scan runs, so obd_link pauses the scan
 * around a connection attempt; scanning during a connection is fine.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "host/ble_gap.h"

// Advertisements, called in the NimBLE host task: keep it short, no blocking.
typedef void (*ble_scan_sink_t)(const struct ble_gap_disc_desc *d);

// Once NimBLE has synced and knows its own address type.
void ble_scan_ready(uint8_t own_addr_type);
// NimBLE reset: the scan is gone until the next ble_scan_ready().
void ble_scan_lost(void);

void ble_scan_want_adapter(bool on);
void ble_scan_want_tpms(bool on);
void ble_scan_set_connected(bool connected);
// Around ble_gap_connect: pause cancels the scan at once.
void ble_scan_pause(void);
void ble_scan_resume(void);

// Re-applies the wanted mode; retries a scan that failed to start. Called
// every ~20 ms from the OBD task, and by the setters above.
void ble_scan_update(void);

void ble_scan_set_adapter_sink(ble_scan_sink_t sink);
void ble_scan_set_tpms_sink(ble_scan_sink_t sink);
