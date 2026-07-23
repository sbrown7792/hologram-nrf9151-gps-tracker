/*
 * Hardware watchdog safety reset (mirrors the original ~10 minute WDT).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_WATCHDOG_H_
#define GPS_TRACKER_WATCHDOG_H_

/**
 * @brief Install and start the watchdog.
 *
 * The timeout is sized to comfortably exceed the battery sleep interval, since
 * the hardware watchdog keeps counting during k_sleep().
 *
 * @return 0 on success, negative errno otherwise.
 */
int watchdog_init(void);

/** @brief Feed (kick) the watchdog. Safe to call if init failed (no-op). */
void watchdog_feed(void);

#endif /* GPS_TRACKER_WATCHDOG_H_ */
