/*
 * Hardware watchdog safety reset. See watchdog.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "watchdog.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/watchdog.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(watchdog, LOG_LEVEL_INF);

/* Cover a full battery sleep plus a generous work-cycle margin. */
#define WDT_TIMEOUT_MS \
	((CONFIG_TRACKER_SLEEP_SECONDS + CONFIG_TRACKER_WATCHDOG_MARGIN_SECONDS) * 1000U)

/* How often the guard timer kicks the watchdog while it is armed. */
#define WDT_GUARD_PERIOD_S 30U

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;

static uint32_t guard_feeds_left;

/* Retained across a reset: .noinit is not cleared by the C startup, and a
 * watchdog reset does not power-cycle the RAM. The magic distinguishes "we came
 * back from a reset and this is where we were" from uninitialised rubbish after
 * a real power-on - or from a build where the retention does not hold, which
 * would otherwise look like a permanent phase 0.
 *
 * volatile because the stores are the entire point: nothing in this file reads
 * phase_current between them, and the compiler would be within its rights to
 * drop all but the last.
 */
#define PHASE_MAGIC 0x57445431u /* "WDT1" */

__noinit static uint32_t phase_magic;
__noinit static volatile uint32_t phase_current;

static enum tracker_phase phase_at_reset;

/* Network calls block the main thread for as long as the protocol stack takes -
 * a confirmable CoAP transfer waits on a semaphore with K_FOREVER - so the
 * caller cannot feed the watchdog itself while one is in flight. The guard
 * feeds on its behalf, but only for a budget: once that runs out we stop, and
 * a genuinely wedged device still gets reset.
 */
static void guard_expiry(struct k_timer *timer)
{
	if (guard_feeds_left == 0) {
		k_timer_stop(timer);
		return;
	}

	guard_feeds_left--;
	watchdog_feed(); /* wdt_feed() is a register write, safe from here. */
}

static K_TIMER_DEFINE(wdt_guard, guard_expiry, NULL);

/* Latch whatever the previous run left behind, then start a fresh trail. Must
 * run before anything calls watchdog_phase().
 */
static void phase_init(void)
{
	if (phase_magic == PHASE_MAGIC) {
		phase_at_reset = (enum tracker_phase)phase_current;

		if (phase_at_reset != TRACKER_PHASE_NONE) {
			LOG_WRN("Restarted from phase %u (%s)", phase_at_reset,
				watchdog_phase_name(phase_at_reset));
		}
	} else {
		/* Either a genuine power-on, or .noinit did not survive. The two
		 * look the same here; forcing a watchdog reset on the bench and
		 * checking for the message above tells them apart.
		 */
		phase_magic = PHASE_MAGIC;
		phase_at_reset = TRACKER_PHASE_NONE;
		LOG_INF("Cold boot, no retained phase marker");
	}

	phase_current = TRACKER_PHASE_NONE;
}

int watchdog_init(void)
{
	phase_init();

	if (!device_is_ready(wdt)) {
		LOG_ERR("Watchdog device not ready");
		return -ENODEV;
	}

	struct wdt_timeout_cfg cfg = {
		.window = {
			.min = 0,
			.max = WDT_TIMEOUT_MS,
		},
		.callback = NULL,
		.flags = WDT_FLAG_RESET_SOC,
	};

	wdt_channel = wdt_install_timeout(wdt, &cfg);
	if (wdt_channel < 0) {
		LOG_ERR("Failed to install watchdog timeout (err %d)", wdt_channel);
		return wdt_channel;
	}

	int err = wdt_setup(wdt, WDT_OPT_PAUSE_HALTED_BY_DBG);

	if (err) {
		LOG_ERR("Failed to set up watchdog (err %d)", err);
		wdt_channel = -1;
		return err;
	}

	LOG_INF("Watchdog started, timeout %u ms", WDT_TIMEOUT_MS);
	return 0;
}

void watchdog_feed(void)
{
	if (wdt_channel >= 0) {
		(void)wdt_feed(wdt, wdt_channel);
	}
}

void watchdog_guard_start(uint32_t budget_s)
{
	guard_feeds_left = DIV_ROUND_UP(budget_s, WDT_GUARD_PERIOD_S);
	watchdog_feed();
	k_timer_start(&wdt_guard, K_SECONDS(WDT_GUARD_PERIOD_S), K_SECONDS(WDT_GUARD_PERIOD_S));
}

void watchdog_guard_stop(void)
{
	k_timer_stop(&wdt_guard);
	watchdog_feed();
}

void watchdog_phase(enum tracker_phase phase)
{
	phase_current = (uint32_t)phase;
}

enum tracker_phase watchdog_reset_phase(void)
{
	return phase_at_reset;
}

const char *watchdog_phase_name(enum tracker_phase phase)
{
	switch (phase) {
	case TRACKER_PHASE_WAKE:
		return "wake";
	case TRACKER_PHASE_LTE:
		return "lte";
	case TRACKER_PHASE_CLOUD_RESUME:
		return "cloud-resume";
	case TRACKER_PHASE_GNSS_START:
		return "gnss-start";
	case TRACKER_PHASE_AGNSS:
		return "agnss";
	case TRACKER_PHASE_FIX:
		return "fix";
	case TRACKER_PHASE_REPORT:
		return "report";
	case TRACKER_PHASE_FREQUENT:
		return "frequent-loop";
	case TRACKER_PHASE_GNSS_STOP:
		return "gnss-stop";
	case TRACKER_PHASE_CLOUD_PAUSE:
		return "cloud-pause";
	case TRACKER_PHASE_LED_SLEEP:
		return "led-sleep";
	case TRACKER_PHASE_MOTION_ARM:
		return "motion-arm";
	case TRACKER_PHASE_SLEEP:
		return "sleep";
	case TRACKER_PHASE_NO_NETWORK:
		return "no-network reboot";
	case TRACKER_PHASE_NONE:
	default:
		return "none";
	}
}
