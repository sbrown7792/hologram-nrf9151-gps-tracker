/*
 * RGB status LED on the nPM1300 LED sinks.
 *
 * The colour carries connectivity:
 *
 *   red    - no LTE registration
 *   yellow - LTE registered, no GNSS fix
 *   green  - LTE registered and GNSS fix
 *
 * and the pattern carries where the power is coming from:
 *
 *   solid          - externally powered, battery full
 *   1 Hz blink     - externally powered, charging
 *   brief flash    - running on battery
 *
 * so "lit continuously" always means plugged in and topped up, and nothing else.
 *
 * The nPM1300 LED outputs are plain on/off current sinks (no brightness), so
 * yellow is simply red and green lit together. All channels must be in "host"
 * mode - see boards/circuitdojo_feather_nrf9151_ns.overlay.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_STATUS_LED_H_
#define GPS_TRACKER_STATUS_LED_H_

#include <stdbool.h>

enum tracker_status {
	TRACKER_STATUS_OFF,        /* dark - sleeping on battery */
	TRACKER_STATUS_NO_LTE,     /* red */
	TRACKER_STATUS_LTE_NO_FIX, /* yellow */
	TRACKER_STATUS_LTE_FIX,    /* green */
};

enum tracker_led_pattern {
	TRACKER_LED_SOLID,    /* externally powered and full */
	TRACKER_LED_CHARGING, /* externally powered and charging */
	TRACKER_LED_BATTERY,  /* on battery */
};

#ifdef CONFIG_TRACKER_STATUS_LED

/**
 * @brief Claim the LED channels and start dark.
 *
 * @return 0 on success, -ENODEV if the PMIC LED driver is unavailable (the
 *         other calls then become no-ops).
 */
int status_led_init(void);

/** @brief Show @p status, keeping the current blink/solid pattern. */
void status_led_set(enum tracker_status status);

/**
 * @brief Select how the status colour is modulated.
 *
 * Runs on the system workqueue, so a pattern continues across reports and
 * sleeps. Must not be called from the system workqueue itself.
 */
void status_led_pattern(enum tracker_led_pattern pattern);

/**
 * @brief Blank the LED before a battery sleep.
 *
 * No-op when TRACKER_STATUS_LED_ON_BATTERY is set, i.e. when carrying the
 * on-battery flash through the whole sleep is worth its share of the sink
 * current.
 */
void status_led_sleep(void);

#else

static inline int status_led_init(void)
{
	return 0;
}

static inline void status_led_set(enum tracker_status status)
{
	(void)status;
}

static inline void status_led_pattern(enum tracker_led_pattern pattern)
{
	(void)pattern;
}

static inline void status_led_sleep(void)
{
}

#endif /* CONFIG_TRACKER_STATUS_LED */

#endif /* GPS_TRACKER_STATUS_LED_H_ */
