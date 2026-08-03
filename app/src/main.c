/*
 * GPS Tracker for the CircuitDojo nRF9151 Feather.
 *
 * Port of the original Hologram Dash sketch (../gps_tracker/gps_tracker.ino):
 *   - acquire a GNSS fix, from an external NMEA module where one is fitted and
 *     otherwise (or on fallback) from the modem's own receiver,
 *   - build the same telemetry JSON the web app expects,
 *   - push it to the configured cloud provider (see cloud.h),
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

#include "agnss.h"
#include "cloud.h"
#include "gnss.h"
#include "motion.h"
#include "power.h"
#include "status_led.h"
#include "telemetry.h"
#include "wake.h"
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

/* Whether a motion surge is still running.
 *
 * Measured from the *last* jolt rather than from the wake, so a tow in progress
 * keeps the tracker reporting for as long as it is being moved, while a one-off
 * false alarm - getting into the car - times out on schedule.
 */
static bool surge_active(bool motion_cycle)
{
	if (!motion_cycle || !IS_ENABLED(CONFIG_TRACKER_MOTION_WAKE)) {
		return false;
	}

	int64_t last = motion_last_event_uptime();

	return last != 0 &&
	       (k_uptime_get() - last) < (CONFIG_TRACKER_MOTION_SURGE_SECONDS * 1000LL);
}

/* Whether an A-GNSS fetch is worth attempting right now. Assistance is injected
 * into the modem, so it is only useful while the onboard receiver is the one
 * running - and only nRF Cloud can serve it, so a "hologram" build never gets
 * past this even with CONFIG_TRACKER_AGNSS compiled in.
 */
static bool agnss_wanted(void)
{
	return IS_ENABLED(CONFIG_TRACKER_AGNSS) && !IS_ENABLED(CONFIG_TRACKER_SKIP_LTE) &&
	       gnss_active_source() == TRACKER_FIX_SOURCE_MODEM && cloud_supports_agnss() &&
	       cloud_is_ready();
}

/* Refresh the status LED: colour from connectivity, blink while charging. In
 * GNSS-only bench mode there is no network by design, so treat the LTE side as
 * satisfied and let the colour track GNSS alone.
 */
#if defined(CONFIG_TRACKER_STATUS_LED)

/* Solid means externally powered and full, and nothing else - so the LED can be
 * read at a glance without knowing what the tracker is up to.
 */
static enum tracker_led_pattern led_pattern(void)
{
	if (!power_vbus_present()) {
		return TRACKER_LED_BATTERY;
	}

	uint16_t batt_mv;
	enum tracker_charge_state charge;

	if (power_read(&batt_mv, &charge) != 0) {
		/* Unknown: show it as still charging rather than claiming full. */
		return TRACKER_LED_CHARGING;
	}

	/* State of charge is the definition of "full" here; the PMIC's own
	 * charge-complete flag is accepted too, as an independent signal that
	 * cannot be wrong when it is set.
	 */
	if (charge == TRACKER_CHARGE_FULL ||
	    telemetry_battery_percent(batt_mv) > CONFIG_TRACKER_STATUS_LED_FULL_PERCENT) {
		return TRACKER_LED_SOLID;
	}

	return TRACKER_LED_CHARGING;
}

#else /* !CONFIG_TRACKER_STATUS_LED */

/* The LED calls are no-op inlines without the LED, but the argument still has to
 * compile, and TRACKER_STATUS_LED_FULL_PERCENT does not exist in that build.
 */
static inline enum tracker_led_pattern led_pattern(void)
{
	return TRACKER_LED_SOLID;
}

#endif /* CONFIG_TRACKER_STATUS_LED */

static void status_led_report(bool have_fix)
{
	bool lte = IS_ENABLED(CONFIG_TRACKER_SKIP_LTE) || lte_is_registered();

	status_led_set(!lte	    ? TRACKER_STATUS_NO_LTE :
		       have_fix	    ? TRACKER_STATUS_LTE_FIX :
				      TRACKER_STATUS_LTE_NO_FIX);
	status_led_pattern(led_pattern());
}

/* Build telemetry from a fix and send it. Returns 0 if the report was sent.
 * A NULL @p fix reports position 0,0 with hdop 0 - used by
 * TRACKER_FORCE_PUBLISH_WITHOUT_FIX when GNSS never locked but the rest of the
 * telemetry (battery, signal, awake time) is still worth sending.
 */
static int report_fix(const struct tracker_fix *fix, uint32_t wake_uptime_ms,
		      enum tracker_wake_reason wake_reason)
{
	struct tracker_telemetry t = {
		.longitude = fix ? fix->longitude : 0.0,
		.latitude = fix ? fix->latitude : 0.0,
		.hdop = fix ? fix->hdop : 0.0f,
		.awake_s = (uint32_t)((k_uptime_get() - wake_uptime_ms) / 1000),
		.vbus = power_vbus_present(),
		.wake = wake_reason,
	};

	if (power_read(&t.batt_mv, &t.charge)) {
		/* batt_mv 0 with charge 0 is the "PMIC unreadable" signature, not a
		 * flat battery on no charger - the two are worth telling apart at
		 * the far end.
		 */
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

	int err = cloud_send_telemetry(payload);

	if (err == -EACCES || err == -ENOTCONN) {
		/* The session can age out server-side while we sleep. Re-establish
		 * once and retry before writing the cycle off.
		 */
		LOG_INF("Cloud link stale, reconnecting for this report");
		watchdog_guard_start(CONFIG_TRACKER_CLOUD_CONNECT_BUDGET_SECONDS);
		err = cloud_resume();
		watchdog_guard_stop();

		if (!err) {
			err = cloud_send_telemetry(payload);
		}
	}

	if (err) {
		LOG_ERR("Telemetry send failed (err %d)", err);
	} else if (fix != NULL) {
		/* Optional, off by default: makes the fix visible on the cloud
		 * provider's own map view. Never sent for a synthesised 0,0.
		 */
		(void)cloud_send_location(fix);
	}

	watchdog_feed();
	return err;
}

/* The battery sleep, shared by the three places that take one.
 *
 * Motion stays armed from here until external power turns up, which is what
 * lets a jolt during the awake part of a cycle still count towards the surge
 * window. Only VBUS disarms it - see the report loop.
 */
static enum tracker_wake_reason battery_sleep(void)
{
	status_led_sleep();
	motion_set_armed(true);

	enum tracker_wake_reason reason = wake_wait(CONFIG_TRACKER_SLEEP_SECONDS);

	if (reason != TRACKER_WAKE_TIMER) {
		LOG_INF("Woke early: %s", wake_reason_name(reason));
	}

	return reason;
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

	if (motion_init()) {
		LOG_WRN("motion_init failed; running without wake-on-motion");
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

	/* Last: needs the modem library up, but no network. Reports a missing
	 * credential at boot rather than after the first failed connect.
	 */
	if (cloud_init()) {
		LOG_ERR("Cloud transport init failed; reports will not be sent");
	}

	status_led_report(false);

	enum tracker_wake_reason wake_reason = TRACKER_WAKE_BOOT;

	while (1) {
		uint32_t wake_uptime_ms = (uint32_t)k_uptime_get();
		bool motion_cycle = wake_reason == TRACKER_WAKE_MOTION;

		watchdog_feed();

		/* Bring up the network (unless in GNSS-only bench mode). On failure,
		 * sleep and retry - the original "sleep 9 min, try again" behavior.
		 */
		if (!IS_ENABLED(CONFIG_TRACKER_SKIP_LTE)) {
			if (lte_ensure_connected() != 0) {
				LOG_WRN("Network unavailable, sleeping before retry");
				status_led_set(TRACKER_STATUS_NO_LTE);
				wake_reason = battery_sleep();
				continue;
			}
		}

		/* Bring the cloud link up before GNSS starts: in LTE-M/GPS
		 * coexistence the modem time-shares the radio, so the handshake is
		 * quickest with the receiver off. It also leaves the session warm
		 * in case the fix attempt below needs assistance data. A failure
		 * here is not fatal - we still try for a fix and retry the link
		 * when the report is actually sent.
		 */
		watchdog_guard_start(CONFIG_TRACKER_CLOUD_CONNECT_BUDGET_SECONDS);
		(void)cloud_resume();
		watchdog_guard_stop();

		/* Registered (or bench mode): yellow until the first fix lands. */
		status_led_report(false);

		/* Start GNSS and get the first fix of this wake cycle. */
		struct tracker_fix fix;
		bool assisted = false;

		if (gnss_start() != 0) {
			cloud_pause();
			wake_reason = battery_sleep();
			continue;
		}

		/* A-GNSS only helps the onboard receiver, and only when it is the
		 * one running - gnss_agnss_request_wait() returns -EAGAIN while the
		 * external module has the cycle, so this whole block is skipped
		 * until (and unless) the fallback below fires. When the modem is
		 * running it asks for assistance within a second or two of starting,
		 * but only when it actually lacks valid data, which makes this a
		 * reliable cold-start test that costs nothing on a warm receiver.
		 */
		if (agnss_wanted() &&
		    gnss_agnss_request_wait(CONFIG_TRACKER_AGNSS_PROACTIVE_WAIT_SECONDS) == 0) {
			LOG_INF("Cold start: fetching A-GNSS assistance up front");
			assisted = agnss_fetch_and_inject() == 0;
		}

		bool have_fix = gnss_wait_fix(&fix, gnss_fix_timeout_seconds()) == 0;

		/* The external module had its window and did not lock. Hand the
		 * cycle to the onboard receiver, which is slower but can be assisted.
		 */
		if (!have_fix && gnss_fallback_available()) {
			LOG_WRN("No fix from the external module, falling back to the "
				"onboard receiver");

			if (gnss_fallback() == 0) {
				if (agnss_wanted() &&
				    gnss_agnss_request_wait(
					    CONFIG_TRACKER_AGNSS_PROACTIVE_WAIT_SECONDS) == 0) {
					LOG_INF("Cold start: fetching A-GNSS assistance");
					assisted = agnss_fetch_and_inject() == 0;
				}

				have_fix = gnss_wait_fix(&fix,
							 gnss_fix_timeout_seconds()) == 0;
			}
		}

		/* Nothing after the full window. If we have not already pulled
		 * assistance this cycle, it is worth a shot before giving up.
		 */
		if (!have_fix && !assisted && agnss_wanted()) {
			LOG_WRN("No fix after %u s, falling back to A-GNSS assistance",
				gnss_fix_timeout_seconds());

			if (agnss_fetch_and_inject() == 0) {
				have_fix = gnss_wait_fix(
					&fix, CONFIG_TRACKER_AGNSS_FIX_TIMEOUT_SECONDS) == 0;
			}
		}
		watchdog_feed();

		if (have_fix) {
			/* Let the fix settle briefly for a better position, then
			 * grab the improved one (original waited ~5 s).
			 */
			if (CONFIG_TRACKER_GNSS_EXTRA_FIX_SECONDS > 0) {
				k_sleep(K_SECONDS(CONFIG_TRACKER_GNSS_EXTRA_FIX_SECONDS));
				(void)gnss_wait_fix(&fix, 5);
			}
			report_fix(&fix, wake_uptime_ms, wake_reason);
			status_led_report(true);
		} else if (IS_ENABLED(CONFIG_TRACKER_FORCE_PUBLISH_WITHOUT_FIX)) {
			LOG_WRN("No GNSS fix this cycle, reporting position 0,0");
			report_fix(NULL, wake_uptime_ms, wake_reason);
			status_led_report(false);
		} else {
			LOG_WRN("No GNSS fix this cycle, skipping report");
			status_led_report(false);
		}
		watchdog_feed();

		/* Frequent reporting, for either of the two reasons we do it: the
		 * car is running, or something just moved the car. They are the
		 * same loop - GNSS stays running so each fix is near-instant - and
		 * differ only in what keeps them going, so a jolt that turns out to
		 * be the owner getting in flows straight into normal powered
		 * operation without a gap.
		 */
		if (power_vbus_present() || surge_active(motion_cycle)) {
			LOG_INF("%s, reporting every %d s (GNSS kept on)",
				power_vbus_present() ? "Externally powered" : "Motion surge",
				CONFIG_TRACKER_CHARGING_INTERVAL_SECONDS);

			while (power_vbus_present() || surge_active(motion_cycle)) {
				/* Watching for jolts is pointless while the car is
				 * running: every bump would raise an event we discard,
				 * each costing a transaction on the PMIC's bus.
				 */
				motion_set_armed(!power_vbus_present());

				k_sleep(K_SECONDS(CONFIG_TRACKER_CHARGING_INTERVAL_SECONDS));
				watchdog_feed();
				if (gnss_wait_fix(&fix, 10) == 0) {
					report_fix(&fix, wake_uptime_ms, wake_reason);
					status_led_report(true);
				} else if (IS_ENABLED(CONFIG_TRACKER_FORCE_PUBLISH_WITHOUT_FIX)) {
					LOG_WRN("No GNSS fix, reporting position 0,0");
					report_fix(NULL, wake_uptime_ms, wake_reason);
					status_led_report(false);
				} else {
					LOG_WRN("No GNSS fix, skipping report");
					status_led_report(false);
				}
			}

			/* Fall through to the sleep below rather than looping straight
			 * into another cycle: the tracker is on battery from this
			 * moment, and an immediate re-acquire would cost a full fix
			 * window (up to the external timeout plus the modem fallback)
			 * with nothing to show for it.
			 */
			LOG_INF("Frequent reporting over, back to the battery cycle");
		}

		/* On battery: stop GNSS and sleep, waking early on external power or
		 * a jolt.
		 */
		gnss_stop();
		/* Keep the session state so the next cycle can resume without paying
		 * for another handshake.
		 */
		cloud_pause();
		LOG_INF("On battery, sleeping up to %d s", CONFIG_TRACKER_SLEEP_SECONDS);
		wake_reason = battery_sleep();
	}

	return 0;
}
