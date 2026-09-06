/*
 * Hardware watchdog safety reset (mirrors the original ~10 minute WDT), plus a
 * breadcrumb saying where the report loop was when it last fired.
 *
 * The watchdog resetting means something blocked for longer than the timeout,
 * and the reset destroys the evidence. The phase marker survives it: it lives in
 * .noinit, which the C startup does not clear and a watchdog reset does not
 * power-cycle, so the value from just before the reset is still there on the way
 * back up. It goes out in the telemetry "wdt" field, because these resets happen
 * in the field where nobody is watching a console.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_WATCHDOG_H_
#define GPS_TRACKER_WATCHDOG_H_

#include <stdint.h>

/* Where the report loop is. Numbers are sent verbatim in telemetry and are
 * matched against this list by hand, so only ever append.
 */
enum tracker_phase {
	TRACKER_PHASE_NONE = 0,	     /* clean boot, or no marker retained */
	TRACKER_PHASE_WAKE = 1,	     /* top of the loop */
	TRACKER_PHASE_LTE = 2,	     /* lte_ensure_connected() */
	TRACKER_PHASE_CLOUD_RESUME = 3,
	TRACKER_PHASE_GNSS_START = 4,
	TRACKER_PHASE_AGNSS = 5,     /* assistance fetch and inject */
	TRACKER_PHASE_FIX = 6,	     /* waiting for a fix */
	TRACKER_PHASE_REPORT = 7,    /* building and sending a report */
	TRACKER_PHASE_FREQUENT = 8,  /* the powered / surge report loop */
	TRACKER_PHASE_GNSS_STOP = 9, /* gnss_stop(), incl. the UART suspend */
	TRACKER_PHASE_CLOUD_PAUSE = 10,
	TRACKER_PHASE_LED_SLEEP = 11,
	TRACKER_PHASE_MOTION_ARM = 12, /* sensor_trigger_set() over I2C */
	TRACKER_PHASE_SLEEP = 13,      /* inside wake_wait() */
	/* Not a place in the loop: set deliberately just before rebooting because
	 * nothing has reached the cloud for too long. See recover_if_silent().
	 */
	TRACKER_PHASE_NO_NETWORK = 14,
};

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

/**
 * @brief Record where the report loop is, for the next reset to report.
 *
 * Cheap - one store to retained RAM. Mark anything that could conceivably
 * block; a phase nobody ever sees costs nothing.
 */
void watchdog_phase(enum tracker_phase phase);

/**
 * @brief The phase the loop was in when the watchdog last reset the device.
 *
 * TRACKER_PHASE_NONE after a clean power-on - and also if the marker did not
 * survive, which watchdog_init() distinguishes in the boot log.
 */
enum tracker_phase watchdog_reset_phase(void);

/** @brief Phase as a short name, for logging. */
const char *watchdog_phase_name(enum tracker_phase phase);

#endif /* GPS_TRACKER_WATCHDOG_H_ */
