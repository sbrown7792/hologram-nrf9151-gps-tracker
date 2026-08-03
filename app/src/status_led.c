/*
 * RGB status LED driven through the nPM1300 LED sinks. See status_led.h.
 *
 * The colour encodes connectivity and the pattern encodes the power source. The
 * PMIC sinks have no hardware blink, so both blink patterns are modulated from
 * the system workqueue and carry on while the tracker reports or sleeps.
 *
 * The on and off phases have separate durations, because the on-battery pattern
 * is a brief flash on a long period rather than a symmetric blink - it has to
 * be cheap enough to leave running.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "status_led.h"

#include <zephyr/device.h>
#include <zephyr/drivers/led.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(status_led, LOG_LEVEL_INF);

#define LED_RED   CONFIG_TRACKER_STATUS_LED_RED_INDEX
#define LED_GREEN CONFIG_TRACKER_STATUS_LED_GREEN_INDEX
#define LED_BLUE  CONFIG_TRACKER_STATUS_LED_BLUE_INDEX

static const struct device *leds = DEVICE_DT_GET(DT_NODELABEL(npm1300_leds));
static bool available;

/* Owned by the calling thread (main) except for `lit`, which the blink work
 * also drives. Only one writer is ever active: blinking is started and stopped
 * from main, and the work is cancelled synchronously before main takes over.
 */
static enum tracker_status current_status = TRACKER_STATUS_OFF;
static enum tracker_led_pattern current_pattern = TRACKER_LED_SOLID;
static bool blinking;
static bool lit;

static void blink_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(blink_work, blink_work_handler);

/* How long to stay in the phase just entered. */
static k_timeout_t phase_delay(bool now_lit)
{
	switch (current_pattern) {
	case TRACKER_LED_CHARGING:
		return K_MSEC(CONFIG_TRACKER_STATUS_LED_BLINK_MS);
	case TRACKER_LED_BATTERY:
		return now_lit ? K_MSEC(CONFIG_TRACKER_STATUS_LED_FLASH_ON_MS)
			       : K_MSEC(CONFIG_TRACKER_STATUS_LED_FLASH_OFF_MS);
	case TRACKER_LED_SOLID:
	default:
		return K_FOREVER;
	}
}

static void set_channel(uint32_t channel, bool on)
{
	int err = on ? led_on(leds, channel) : led_off(leds, channel);

	/* -EPERM means the channel is not in "host" mode - the board overlay is
	 * missing or was overridden.
	 */
	if (err) {
		LOG_WRN("LED channel %u %s failed (err %d)", channel,
			on ? "on" : "off", err);
	}
}

/* Drive the channels for the current status, or dark when in a blink's off
 * phase. Yellow is red + green together - the sinks are on/off only.
 */
static void apply(bool on)
{
	lit = on;

	bool red = on && (current_status == TRACKER_STATUS_NO_LTE ||
			  current_status == TRACKER_STATUS_LTE_NO_FIX);
	bool green = on && (current_status == TRACKER_STATUS_LTE_FIX ||
			    current_status == TRACKER_STATUS_LTE_NO_FIX);

	set_channel(LED_RED, red);
	set_channel(LED_GREEN, green);
}

static void blink_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);

	apply(!lit);
	k_work_reschedule(&blink_work, phase_delay(lit));
}

/* Stop blinking and make sure the handler is not mid-run, so the caller is the
 * only writer afterwards. Must not be called from the system workqueue.
 */
static void stop_blink(void)
{
	static struct k_work_sync sync;

	blinking = false;
	(void)k_work_cancel_delayable_sync(&blink_work, &sync);
}

int status_led_init(void)
{
	if (!device_is_ready(leds)) {
		LOG_ERR("nPM1300 LED driver not ready; status LED disabled");
		return -ENODEV;
	}

	available = true;

	/* Blue is unused by the status colours; make sure nothing else left it on. */
	set_channel(LED_BLUE, false);
	status_led_set(TRACKER_STATUS_OFF);

	return 0;
}

void status_led_set(enum tracker_status status)
{
	if (!available) {
		return;
	}

	current_status = status;

	if (status == TRACKER_STATUS_OFF) {
		/* Nothing to modulate. */
		stop_blink();
	}

	/* Restart the blink lit-first so a status change is visible immediately. */
	apply(true);

	if (blinking) {
		k_work_reschedule(&blink_work, phase_delay(true));
	}
}

void status_led_pattern(enum tracker_led_pattern pattern)
{
	if (!available) {
		return;
	}

	bool want_blink = pattern != TRACKER_LED_SOLID;

	/* Compare against what the LED is actually doing, not just the last
	 * pattern asked for: status_led_sleep() cancels the blink work without
	 * changing the pattern, so on waking the two disagree and the pattern has
	 * to be re-established.
	 */
	if (pattern == current_pattern && want_blink == blinking) {
		return;
	}

	current_pattern = pattern;

	if (pattern == TRACKER_LED_SOLID) {
		stop_blink();
		apply(true);
		return;
	}

	/* Start lit so the change is visible at once rather than after a whole
	 * off phase - which for the battery flash would be several seconds.
	 */
	blinking = true;
	apply(true);
	k_work_reschedule(&blink_work, phase_delay(true));
}

void status_led_sleep(void)
{
	if (!IS_ENABLED(CONFIG_TRACKER_STATUS_LED_ON_BATTERY)) {
		status_led_set(TRACKER_STATUS_OFF);
	}
}
