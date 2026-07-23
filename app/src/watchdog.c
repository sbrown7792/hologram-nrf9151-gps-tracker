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
#define WDT_TIMEOUT_MS ((CONFIG_TRACKER_SLEEP_SECONDS + 180) * 1000U)

static const struct device *const wdt = DEVICE_DT_GET(DT_ALIAS(watchdog0));
static int wdt_channel = -1;

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
