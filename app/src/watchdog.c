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

int watchdog_init(void)
{
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
