/*
 * Hardware watchdog safety reset (mirrors the original ~10 minute WDT).
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_WATCHDOG_H_
#define GPS_TRACKER_WATCHDOG_H_

#include <stdint.h>

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

/**
 * @brief Feed the watchdog automatically for at most @p budget_s.
 *
 * For blocking calls the caller cannot interrupt to feed the watchdog itself -
 * network I/O, principally. The guard keeps the watchdog fed from a timer, but
 * only until the budget is spent; after that it stops feeding, so a call that
 * never returns still resets the device.
 *
 * Always pair with watchdog_guard_stop(). Not nestable.
 *
 * @param budget_s  How long the wrapped operation is allowed to take.
 */
void watchdog_guard_start(uint32_t budget_s);

/** @brief Disarm the guard and feed once. Safe if no guard is armed. */
void watchdog_guard_stop(void);

#endif /* GPS_TRACKER_WATCHDOG_H_ */
