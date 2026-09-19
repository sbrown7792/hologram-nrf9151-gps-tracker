/*
 * What ended a battery sleep. See wake.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "wake.h"

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

/* Binary: several signals before anyone waits collapse into one wake, which is
 * what we want - the reason is read from `pending` rather than counted.
 */
static K_SEM_DEFINE(wake_sem, 0, 1);
static atomic_t pending = ATOMIC_INIT((atomic_val_t)TRACKER_WAKE_TIMER);

void wake_signal(enum tracker_wake_reason reason)
{
	atomic_set(&pending, (atomic_val_t)reason);
	k_sem_give(&wake_sem);
}

enum tracker_wake_reason wake_wait(uint32_t seconds)
{
	/* Drop anything raised before we started waiting: a jolt during the
	 * report we just sent has already been acted on by that report.
	 */
	atomic_set(&pending, (atomic_val_t)TRACKER_WAKE_TIMER);
	k_sem_reset(&wake_sem);

	if (k_sem_take(&wake_sem, K_SECONDS(seconds)) != 0) {
		return TRACKER_WAKE_TIMER;
	}

	return (enum tracker_wake_reason)atomic_get(&pending);
}

const char *wake_reason_name(enum tracker_wake_reason reason)
{
	switch (reason) {
	case TRACKER_WAKE_BOOT:
		return "boot";
	case TRACKER_WAKE_VBUS:
		return "vbus";
	case TRACKER_WAKE_MOTION:
		return "motion";
	case TRACKER_WAKE_TIMER:
	default:
		return "timer";
	}
}
