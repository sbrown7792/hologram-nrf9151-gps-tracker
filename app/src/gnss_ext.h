/*
 * External NMEA GNSS module backend (Adafruit Ultimate GPS v3 / MTK3339).
 *
 * One of the two receivers behind gnss.h; the arbiter in gnss.c decides when it
 * runs. Never called from main.c directly.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_GNSS_EXT_H_
#define GPS_TRACKER_GNSS_EXT_H_

#include <stdbool.h>
#include <stdint.h>

#include "fix.h"

/**
 * @brief Claim the power-control GPIO and leave the module switched off.
 *
 * Call once at startup. The UART and the NMEA driver are left suspended until
 * gnss_ext_start().
 *
 * @return 0 on success, negative errno otherwise.
 */
int gnss_ext_init(void);

/**
 * @brief Power the module up and start consuming NMEA.
 *
 * Blocks for CONFIG_TRACKER_GNSS_EXT_WARMUP_MS while the module boots.
 *
 * @return 0 on success, negative errno otherwise.
 */
int gnss_ext_start(void);

/**
 * @brief Stop consuming NMEA.
 *
 * The UART is always suspended, which both drops its idle current and parks the
 * pins so the TX line cannot back-feed an unpowered module.
 *
 * @param power_off  Whether to open the FET as well. Leaving the module powered
 *                   keeps it locked, so the next fix is instant at the cost of
 *                   its running current - the caller owns that tradeoff.
 */
void gnss_ext_stop(bool power_off);

/**
 * @brief Block until the module reports a valid fix, or timeout.
 *
 * @param out        Filled with the fix on success.
 * @param timeout_s  Maximum time to wait, seconds.
 * @return 0 on success, -EAGAIN on timeout.
 */
int gnss_ext_wait_fix(struct tracker_fix *out, uint32_t timeout_s);

#endif /* GPS_TRACKER_GNSS_EXT_H_ */
