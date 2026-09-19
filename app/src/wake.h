/*
 * What ended a battery sleep.
 *
 * Two independent sources can cut a sleep short - external power arriving
 * (power.c, via the nPM1300 VBUS event) and a jolt on the accelerometer
 * (motion.c) - so the semaphore they share lives here rather than in either of
 * them, and carries the reason across to the report loop.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_WAKE_H_
#define GPS_TRACKER_WAKE_H_

#include <stdint.h>

enum tracker_wake_reason {
	TRACKER_WAKE_BOOT,   /* first cycle after a reset, watchdog resets included */
	TRACKER_WAKE_TIMER,  /* the sleep simply ran its course */
	TRACKER_WAKE_VBUS,   /* external power applied */
	TRACKER_WAKE_MOTION, /* accelerometer jolt */
};

/**
 * @brief End the current wake_wait() early.
 *
 * Safe from any thread context, including the system workqueue where both the
 * PMIC event callback and the accelerometer trigger handler run. Signals raised
 * while nothing is waiting are discarded by the next wake_wait().
 *
 * @param reason  What happened, returned to the waiter.
 */
void wake_signal(enum tracker_wake_reason reason);

/**
 * @brief Sleep for @p seconds, or until wake_signal() is called.
 *
 * @return TRACKER_WAKE_TIMER if the full time elapsed, otherwise the reason
 *         passed to wake_signal().
 */
enum tracker_wake_reason wake_wait(uint32_t seconds);

/** @brief Reason as the short string used in logs and the telemetry payload. */
const char *wake_reason_name(enum tracker_wake_reason reason);

#endif /* GPS_TRACKER_WAKE_H_ */
