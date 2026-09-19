/*
 * Wake-on-motion via the onboard LIS2DH accelerometer.
 *
 * Detects a jolt - a tow hookup, an impact, someone getting in - while the
 * tracker is asleep on battery, and pulls it into frequent reporting. The
 * accelerometer raises a hardware interrupt on its own, so nothing has to stay
 * awake to poll it.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_MOTION_H_
#define GPS_TRACKER_MOTION_H_

#include <stdbool.h>
#include <stdint.h>

#if defined(CONFIG_TRACKER_MOTION_WAKE)

/**
 * @brief Configure the accelerometer's any-motion detector. Call once at startup.
 *
 * Leaves the trigger disarmed; nothing is reported until motion_set_armed(true).
 *
 * @return 0 on success, negative errno otherwise. A failure is not fatal - the
 *         caller can carry on without motion wake.
 */
int motion_init(void);

/**
 * @brief Arm or disarm the jolt interrupt.
 *
 * Idempotent, so it is cheap to call on every pass of the report loop: only a
 * real change touches the sensor over I2C.
 *
 * Motion is only worth watching on battery. While the car is running the
 * accelerometer would raise a continuous stream of events that all get
 * discarded, each one costing a bus transaction shared with the PMIC.
 */
void motion_set_armed(bool armed);

/**
 * @brief Uptime in milliseconds of the most recent jolt, or 0 if none yet.
 *
 * The report loop measures the surge window from this rather than from the wake
 * itself, so continued jolts extend it.
 */
int64_t motion_last_event_uptime(void);

#else /* !CONFIG_TRACKER_MOTION_WAKE */

static inline int motion_init(void)
{
	return 0;
}

static inline void motion_set_armed(bool armed)
{
	(void)armed;
}

static inline int64_t motion_last_event_uptime(void)
{
	return 0;
}

#endif /* CONFIG_TRACKER_MOTION_WAKE */

#endif /* GPS_TRACKER_MOTION_H_ */
