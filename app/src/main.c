/*
 * GPS Tracker for the CircuitDojo nRF9151 Feather.
 *
 * Port of the original Hologram Dash sketch (../gps_tracker/gps_tracker.ino):
 *   - acquire a GNSS fix,
 *   - build the same telemetry JSON the web app expects,
 *   - push it into the Hologram Data Engine via the Cloud Socket API,
 *   - report every ~60 s while externally powered, otherwise sleep ~9 min,
 *   - wake early from the battery sleep when external power is applied,
 *   - hardware watchdog as a safety reset,
 *   - RGB status LED: red = no LTE, yellow = LTE only, green = LTE + fix,
 *     blinking while charging.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <modem/nrf_modem_lib.h>

#include "gnss.h"
#include "hologram.h"
#include "power.h"
#include "status_led.h"
#include "telemetry.h"
#include "watchdog.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

static K_SEM_DEFINE(lte_connected_sem, 0, 1);

static void lte_handler(const struct lte_lc_evt *const evt)
{
	if (evt->type == LTE_LC_EVT_NW_REG_STATUS &&
	    (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
	     evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING)) {
		LOG_INF("LTE registered (%s)",
			evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING ?
				"roaming" : "home");
		k_sem_give(&lte_connected_sem);
	}
}

static bool lte_is_registered(void)
{
	enum lte_lc_nw_reg_status status;

	if (lte_lc_nw_reg_status_get(&status) != 0) {
		return false;
	}

	return status == LTE_LC_NW_REG_REGISTERED_HOME ||
	       status == LTE_LC_NW_REG_REGISTERED_ROAMING;
}

/* Ensure the modem is registered. Returns 0 if registered within the timeout,
 * else -ETIMEDOUT. Never blocks forever, so a missing/unregistered SIM degrades
 * gracefully. PSM keeps the modem registered between cycles, so this is usually
 * an immediate no-op after the first connect.
 */
static int lte_ensure_connected(void)
{
	if (lte_is_registered()) {
		return 0;
	}

	k_sem_reset(&lte_connected_sem);

	/* Handler already registered via lte_lc_register_handler(). */
	int err = lte_lc_connect_async(NULL);

	if (err && err != -EINPROGRESS) {
		LOG_ERR("lte_lc_connect_async failed (err %d)", err);
		return err;
	}

	err = k_sem_take(&lte_connected_sem,
			 K_SECONDS(CONFIG_TRACKER_LTE_CONNECT_TIMEOUT_SECONDS));
	if (err) {
		LOG_WRN("LTE registration timed out after %d s",
			CONFIG_TRACKER_LTE_CONNECT_TIMEOUT_SECONDS);
		return -ETIMEDOUT;
	}

	return 0;
}

/* Refresh the status LED: colour from connectivity, blink while charging. In
 * GNSS-only bench mode there is no network by design, so treat the LTE side as
 * satisfied and let the colour track GNSS alone.
 */
static void status_led_report(bool have_fix)
{
	bool lte = IS_ENABLED(CONFIG_TRACKER_SKIP_LTE) || lte_is_registered();

	status_led_set(!lte	    ? TRACKER_STATUS_NO_LTE :
		       have_fix	    ? TRACKER_STATUS_LTE_FIX :
				      TRACKER_STATUS_LTE_NO_FIX);
	status_led_charging(power_charge_state() == TRACKER_CHARGE_CHARGING);
}

/* Build telemetry from a fix and send it. Returns 0 if the report was sent.
 * A NULL @p pvt reports position 0,0 with hdop 0 - used by
 * TRACKER_FORCE_PUBLISH_WITHOUT_FIX when GNSS never locked but the rest of the
 * telemetry (battery, signal, awake time) is still worth sending.
 */
static int report_fix(const struct nrf_modem_gnss_pvt_data_frame *pvt,
		      uint32_t wake_uptime_ms)
{
	struct tracker_telemetry t = {
		.longitude = pvt ? pvt->longitude : 0.0,
		.latitude = pvt ? pvt->latitude : 0.0,
		.hdop = pvt ? pvt->hdop : 0.0f,
		.awake_s = (uint32_t)((k_uptime_get() - wake_uptime_ms) / 1000),
	};

	if (power_read(&t.batt_mv, &t.charge)) {
		t.batt_mv = 0;
		t.charge = TRACKER_CHARGE_DISCHARGING;
	}

	if (modem_info_get_rsrp(&t.signal_dbm)) {
		t.signal_dbm = 0; /* no valid RSRP (e.g. GNSS-only / no network) */
	}

	char payload[256];

	if (telemetry_build_json(&t, payload, sizeof(payload)) < 0) {
		LOG_ERR("Telemetry payload too large");
		return -ENOMEM;
	}

	int err = hologram_send(payload);

	if (err) {
		LOG_ERR("Hologram send failed (err %d)", err);
	}

	watchdog_feed();
	return err;
}

int main(void)
{
	int err;

	LOG_INF("GPS Tracker starting");

	err = nrf_modem_lib_init();
	if (err) {
		LOG_ERR("Modem library init failed (err %d)", err);
		return err;
	}

	(void)watchdog_init();

	if (modem_info_init()) {
		LOG_WRN("modem_info_init failed; battery/signal may be unavailable");
	}

	if (power_init()) {
		LOG_WRN("power_init failed; running without charge detection");
	}

	(void)status_led_init();

	if (gnss_init()) {
		LOG_ERR("GNSS init failed");
		return -EIO;
	}

	if (IS_ENABLED(CONFIG_TRACKER_SKIP_LTE)) {
		LOG_WRN("TRACKER_SKIP_LTE: GNSS-only mode, no cellular");
		if (lte_lc_func_mode_set(LTE_LC_FUNC_MODE_ACTIVATE_GNSS)) {
			LOG_ERR("Failed to activate GNSS-only functional mode");
			return -EIO;
		}
	} else {
		lte_lc_register_handler(lte_handler);
		(void)lte_lc_psm_req(true);
	}

	status_led_report(false);

	while (1) {
		uint32_t wake_uptime_ms = (uint32_t)k_uptime_get();

		watchdog_feed();

		/* Bring up the network (unless in GNSS-only bench mode). On failure,
		 * sleep and retry - the original "sleep 9 min, try again" behavior.
		 */
		if (!IS_ENABLED(CONFIG_TRACKER_SKIP_LTE)) {
			if (lte_ensure_connected() != 0) {
				LOG_WRN("Network unavailable, sleeping before retry");
				status_led_set(TRACKER_STATUS_NO_LTE);
				status_led_sleep();
				power_wait_interruptible(CONFIG_TRACKER_SLEEP_SECONDS);
				continue;
			}
		}

		/* Registered (or bench mode): yellow until the first fix lands. */
		status_led_report(false);

		/* Start GNSS and get the first fix of this wake cycle. */
		struct nrf_modem_gnss_pvt_data_frame pvt;

		if (gnss_start() != 0) {
			status_led_sleep();
			power_wait_interruptible(CONFIG_TRACKER_SLEEP_SECONDS);
			continue;
		}

		if (gnss_wait_fix(&pvt, CONFIG_TRACKER_GNSS_FIX_TIMEOUT_SECONDS) == 0) {
			/* Let the fix settle briefly for a better position, then
			 * grab the improved one (original waited ~5 s).
			 */
			if (CONFIG_TRACKER_GNSS_EXTRA_FIX_SECONDS > 0) {
				k_sleep(K_SECONDS(CONFIG_TRACKER_GNSS_EXTRA_FIX_SECONDS));
				(void)gnss_wait_fix(&pvt, 5);
			}
			report_fix(&pvt, wake_uptime_ms);
			status_led_report(true);
		} else if (IS_ENABLED(CONFIG_TRACKER_FORCE_PUBLISH_WITHOUT_FIX)) {
			LOG_WRN("No GNSS fix this cycle, reporting position 0,0");
			report_fix(NULL, wake_uptime_ms);
			status_led_report(false);
		} else {
			LOG_WRN("No GNSS fix this cycle, skipping report");
			status_led_report(false);
		}
		watchdog_feed();

		if (power_is_charging()) {
			/* Externally powered: keep GNSS running (stays locked) so
			 * each subsequent fix is near-instant.
			 */
			LOG_INF("Externally powered, reporting every %d s (GNSS kept on)",
				CONFIG_TRACKER_CHARGING_INTERVAL_SECONDS);
			while (power_is_charging()) {
				k_sleep(K_SECONDS(CONFIG_TRACKER_CHARGING_INTERVAL_SECONDS));
				watchdog_feed();
				if (gnss_wait_fix(&pvt, 10) == 0) {
					report_fix(&pvt, wake_uptime_ms);
					status_led_report(true);
				} else if (IS_ENABLED(CONFIG_TRACKER_FORCE_PUBLISH_WITHOUT_FIX)) {
					LOG_WRN("No GNSS fix, reporting position 0,0");
					report_fix(NULL, wake_uptime_ms);
					status_led_report(false);
				} else {
					LOG_WRN("No GNSS fix, skipping report");
					status_led_report(false);
				}
			}
			gnss_stop();
		} else {
			/* On battery: stop GNSS and sleep, waking early on external power. */
			gnss_stop();
			LOG_INF("On battery, sleeping up to %d s",
				CONFIG_TRACKER_SLEEP_SECONDS);
			status_led_sleep();
			if (power_wait_interruptible(CONFIG_TRACKER_SLEEP_SECONDS)) {
				LOG_INF("External power applied, waking early");
			}
		}
	}

	return 0;
}
