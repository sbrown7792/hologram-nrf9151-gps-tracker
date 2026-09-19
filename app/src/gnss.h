/*
 * GNSS fix acquisition, independent of which receiver is used.
 *
 * Two backends can be compiled in (see CMakeLists.txt):
 *
 *   gnss_modem.c  nRF9151 onboard GNSS   (always)
 *   gnss_ext.c    external NMEA module   (CONFIG_TRACKER_GNSS_EXTERNAL)
 *
 * When both are present the external module is the preferred source, because it
 * acquires far faster than the modem receiver does; gnss_fallback() hands the
 * cycle over to the onboard receiver when the external module fails to lock,
 * which is also the only way A-GNSS assistance comes into play.
 *
 * Lifecycle is split from reading so the receiver can be kept running (locked)
 * across multiple reports - important while externally powered, where we want a
 * near-instant fix every cycle instead of re-acquiring each time.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_GNSS_H_
#define GPS_TRACKER_GNSS_H_

#include <stdbool.h>
#include <stdint.h>

#include <nrf_modem_gnss.h>

#include "fix.h"

/**
 * @brief Initialise every compiled-in receiver. Call once at startup.
 *
 * The modem library must already be initialised. Leaves all receivers stopped
 * and, for the external module, unpowered.
 *
 * @return 0 if at least the preferred receiver came up, negative errno if none
 *         did.
 */
int gnss_init(void);

/**
 * @brief Start the preferred receiver for a new report cycle.
 *
 * Resets the active source back to the preferred one, so a cycle that fell back
 * to the modem does not leave the next cycle stuck there.
 *
 * @return 0 on success, negative errno otherwise.
 */
int gnss_start(void);

/** @brief Stop the active receiver. */
void gnss_stop(void);

/**
 * @brief Block until the active receiver reports a valid fix, or timeout.
 *
 * With the receiver already locked this returns within ~1 s. Does not start or
 * stop anything.
 *
 * @param out        Filled with the fix on success.
 * @param timeout_s  Maximum time to wait for a valid fix, seconds.
 * @return 0 on success, -EAGAIN on timeout.
 */
int gnss_wait_fix(struct tracker_fix *out, uint32_t timeout_s);

/** @brief Which receiver is currently running. */
enum tracker_fix_source gnss_active_source(void);

/**
 * @brief The fix window that suits the active receiver, in seconds.
 *
 * The external module is given a shorter window than the modem, since if it has
 * not locked by then the fallback is more likely to pay off than more waiting.
 */
uint32_t gnss_fix_timeout_seconds(void);

/** @brief Whether gnss_fallback() has anywhere left to go this cycle. */
bool gnss_fallback_available(void);

/**
 * @brief Hand the cycle over to the onboard receiver.
 *
 * Powers the external module down and starts the modem receiver, after which
 * gnss_agnss_request_wait() becomes meaningful and A-GNSS assistance can be
 * fetched.
 *
 * @return 0 on success, -ENOTSUP if there is no fallback, negative errno if the
 *         modem receiver would not start.
 */
int gnss_fallback(void);

/**
 * @brief Fetch the assistance data the modem receiver last asked for.
 *
 * Populated from NRF_MODEM_GNSS_EVT_AGNSS_REQ. The frame is sticky: it reflects
 * the most recent request rather than being a one-shot queue, because the modem
 * re-raises the event whenever it still needs data.
 *
 * @param out  Filled on success.
 * @return true if the modem receiver has requested assistance since boot, false
 *         otherwise (including whenever it is not the active source).
 */
bool gnss_agnss_request_get(struct nrf_modem_gnss_agnss_data_frame *out);

/**
 * @brief Block until the modem receiver asks for assistance, or timeout.
 *
 * The modem raises the request shortly after it starts only when it lacks valid
 * assistance data, which makes this a reliable cold-start test: a warm receiver
 * simply never fires it. Only requests raised since the most recent start are
 * counted, and it returns -EAGAIN immediately while the external module is the
 * active source.
 *
 * Retrieve what was asked for with gnss_agnss_request_get().
 *
 * @param timeout_s  Maximum time to wait, seconds.
 * @return 0 if assistance was requested, -EAGAIN on timeout.
 */
int gnss_agnss_request_wait(uint32_t timeout_s);

#endif /* GPS_TRACKER_GNSS_H_ */
