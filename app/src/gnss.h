/*
 * GNSS fix acquisition using the nRF91 onboard GNSS (nrf_modem_gnss).
 *
 * Lifecycle is split from reading so the receiver can be kept running (locked)
 * across multiple reports - important while externally powered, where we want a
 * near-instant fix every cycle instead of re-acquiring each time.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_GNSS_H_
#define GPS_TRACKER_GNSS_H_

#include <nrf_modem_gnss.h>

/**
 * @brief Register the GNSS event handler. Call once at startup.
 *
 * The modem library must already be initialised. GNSS parameter configuration
 * is deferred to gnss_start() because it requires GNSS to be active.
 */
int gnss_init(void);

/**
 * @brief Configure and start continuous (1 Hz) GNSS tracking.
 *
 * GNSS must be active in the current functional mode (NORMAL with GPS, or
 * ACTIVATE_GNSS) before calling. Leaves the receiver running until gnss_stop().
 *
 * @return 0 on success, negative errno otherwise.
 */
int gnss_start(void);

/** @brief Stop the GNSS receiver. */
void gnss_stop(void);

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
int gnss_wait_fix(struct nrf_modem_gnss_pvt_data_frame *out, uint32_t timeout_s);

#endif /* GPS_TRACKER_GNSS_H_ */
