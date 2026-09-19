/*
 * Onboard nRF9151 GNSS backend (nrf_modem_gnss).
 *
 * One of the two receivers behind gnss.h; the arbiter in gnss.c decides when it
 * runs. Never called from main.c directly.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_GNSS_MODEM_H_
#define GPS_TRACKER_GNSS_MODEM_H_

#include <stdbool.h>
#include <stdint.h>

#include <nrf_modem_gnss.h>

#include "fix.h"

/**
 * @brief Register the GNSS event handler. Call once at startup.
 *
 * The modem library must already be initialised. GNSS parameter configuration
 * is deferred to gnss_modem_start() because it requires GNSS to be active.
 */
int gnss_modem_init(void);

/**
 * @brief Configure and start continuous (1 Hz) GNSS tracking.
 *
 * GNSS must be active in the current functional mode (NORMAL with GPS, or
 * ACTIVATE_GNSS) before calling. Leaves the receiver running until
 * gnss_modem_stop().
 *
 * @return 0 on success, negative errno otherwise.
 */
int gnss_modem_start(void);

/** @brief Stop the GNSS receiver. */
void gnss_modem_stop(void);

/**
 * @brief Block until the next valid fix, or timeout.
 *
 * With the receiver already locked (continuous tracking) this returns within
 * ~1 s. Does not start or stop GNSS.
 *
 * @param out        Filled with the fix on success.
 * @param timeout_s  Maximum time to wait for a valid fix, seconds.
 * @return 0 on success, -EAGAIN on timeout.
 */
int gnss_modem_wait_fix(struct tracker_fix *out, uint32_t timeout_s);

/**
 * @brief Fetch the assistance data GNSS last asked for.
 *
 * Populated from NRF_MODEM_GNSS_EVT_AGNSS_REQ. The frame is sticky: it reflects
 * the most recent request rather than being a one-shot queue, because the modem
 * re-raises the event whenever it still needs data.
 *
 * @param out  Filled on success.
 * @return true if GNSS has requested assistance since boot, false otherwise.
 */
bool gnss_modem_agnss_request_get(struct nrf_modem_gnss_agnss_data_frame *out);

/**
 * @brief Block until GNSS asks for assistance, or timeout.
 *
 * The modem raises the request shortly after gnss_modem_start() only when it
 * lacks valid assistance data, which makes this a reliable cold-start test: a
 * warm receiver simply never fires it. Only requests raised since the most
 * recent gnss_modem_start() are counted.
 *
 * @param timeout_s  Maximum time to wait, seconds.
 * @return 0 if assistance was requested, -EAGAIN on timeout.
 */
int gnss_modem_agnss_request_wait(uint32_t timeout_s);

#endif /* GPS_TRACKER_GNSS_MODEM_H_ */
