/*
 * The BLE side: find the ELM327 adapter, connect, find its UART-over-GATT
 * service, subscribe to notifications and move bytes. NimBLE callbacks run in
 * the NimBLE host task; they only post events to the OBD task's queue, which
 * runs the session. Adapter layouts: Vgate 18F0/2AF0/2AF1 first, then the
 * FFF0 and FFE0 clones.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    LINK_EV_UP,       // subscribed: the session can start
    LINK_EV_DOWN,     // the link is gone
    LINK_EV_DATA,     // notification bytes
    LINK_EV_PHASE,    // data[0] = obd_phase_t (scan / connect) for the UI
} link_ev_type_t;

#define LINK_EV_MAX 120

typedef struct {
    link_ev_type_t type;
    uint8_t len;
    uint8_t data[LINK_EV_MAX];
} link_event_t;

// Start the NimBLE host. Events go to `events`.
void obd_link_init(QueueHandle_t events);
// Begin (or stop) looking for the adapter.
void obd_link_start(void);
void obd_link_stop(void);
// From the OBD task every ~20 ms: paces scan and connect attempts.
void obd_link_poll(int64_t now);
// Queue bytes for the adapter. False if the link is not ready.
bool obd_link_send(const uint8_t *data, int len);
// Drop the link and look again (the session asks after repeated timeouts).
void obd_link_restart(void);
// Forget the remembered adapter.
void obd_link_forget(void);
// The adapter's advertised name, "" until found.
const char *obd_link_name(void);
