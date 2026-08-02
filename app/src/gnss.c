/*
 * Arbiter between the compiled-in GNSS receivers.
 *
 * Holds a single piece of state - which receiver is active - and forwards the
 * gnss.h calls to it. Everything receiver-specific lives in gnss_modem.c and
 * gnss_ext.c; main.c sees only gnss.h.
 *
 * The preference is fixed rather than adaptive: the external module is tried
 * first every cycle, and a cycle that had to fall back does not make the next
 * one start on the modem. The external module is the faster receiver whenever
 * it can see the sky at all, so a single bad cycle is not evidence worth
 * carrying forward - and re-trying it costs only its (shorter) fix window.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "gnss_modem.h"

#if defined(CONFIG_TRACKER_GNSS_EXTERNAL)
#include "gnss_ext.h"
#endif

LOG_MODULE_REGISTER(gnss, LOG_LEVEL_INF);

#define EXTERNAL_PREFERRED IS_ENABLED(CONFIG_TRACKER_GNSS_EXTERNAL)

static enum tracker_fix_source active = TRACKER_FIX_SOURCE_MODEM;

/* Set when the external module is compiled in but did not initialise, so the
 * tracker degrades to the onboard receiver instead of never reporting.
 */
static bool external_usable;

int gnss_init(void)
{
	int modem_err = gnss_modem_init();

	if (modem_err) {
		LOG_ERR("Onboard GNSS init failed (err %d)", modem_err);
	}

#if defined(CONFIG_TRACKER_GNSS_EXTERNAL)
	int ext_err = gnss_ext_init();

	if (ext_err) {
		LOG_ERR("External GNSS init failed (err %d); using the onboard receiver",
			ext_err);
	} else {
		external_usable = true;
	}
#endif

	if (!external_usable && modem_err) {
		return modem_err;
	}

	active = external_usable ? TRACKER_FIX_SOURCE_EXTERNAL : TRACKER_FIX_SOURCE_MODEM;
	return 0;
}

int gnss_start(void)
{
#if defined(CONFIG_TRACKER_GNSS_EXTERNAL)
	if (external_usable) {
		active = TRACKER_FIX_SOURCE_EXTERNAL;

		int err = gnss_ext_start();

		if (err == 0) {
			return 0;
		}

		/* Not fatal on its own - the onboard receiver is still there. */
		LOG_ERR("External GNSS failed to start (err %d), using the onboard receiver",
			err);
	}
#endif

	active = TRACKER_FIX_SOURCE_MODEM;
	return gnss_modem_start();
}

void gnss_stop(void)
{
#if defined(CONFIG_TRACKER_GNSS_EXTERNAL)
	if (active == TRACKER_FIX_SOURCE_EXTERNAL) {
		gnss_ext_stop(IS_ENABLED(CONFIG_TRACKER_GNSS_EXT_POWER_CYCLE));
		return;
	}
#endif

	gnss_modem_stop();
}

int gnss_wait_fix(struct tracker_fix *out, uint32_t timeout_s)
{
#if defined(CONFIG_TRACKER_GNSS_EXTERNAL)
	if (active == TRACKER_FIX_SOURCE_EXTERNAL) {
		return gnss_ext_wait_fix(out, timeout_s);
	}
#endif

	return gnss_modem_wait_fix(out, timeout_s);
}

enum tracker_fix_source gnss_active_source(void)
{
	return active;
}

uint32_t gnss_fix_timeout_seconds(void)
{
#if defined(CONFIG_TRACKER_GNSS_EXTERNAL)
	if (active == TRACKER_FIX_SOURCE_EXTERNAL) {
		return CONFIG_TRACKER_GNSS_EXT_FIX_TIMEOUT_SECONDS;
	}
#endif

	return CONFIG_TRACKER_GNSS_FIX_TIMEOUT_SECONDS;
}

bool gnss_fallback_available(void)
{
	return EXTERNAL_PREFERRED && active == TRACKER_FIX_SOURCE_EXTERNAL &&
	       IS_ENABLED(CONFIG_TRACKER_GNSS_EXT_MODEM_FALLBACK);
}

int gnss_fallback(void)
{
	if (!gnss_fallback_available()) {
		return -ENOTSUP;
	}

#if defined(CONFIG_TRACKER_GNSS_EXTERNAL)
	/* Cut power regardless of TRACKER_GNSS_EXT_POWER_CYCLE: the module has
	 * already had its whole fix window and clearly is not going to lock, so
	 * keeping it running for the rest of the cycle only costs current.
	 */
	gnss_ext_stop(true);
#endif

	active = TRACKER_FIX_SOURCE_MODEM;

	int err = gnss_modem_start();

	if (err) {
		LOG_ERR("Fallback to the onboard receiver failed (err %d)", err);
		return err;
	}

	return 0;
}

bool gnss_agnss_request_get(struct nrf_modem_gnss_agnss_data_frame *out)
{
	if (active != TRACKER_FIX_SOURCE_MODEM) {
		return false;
	}

	return gnss_modem_agnss_request_get(out);
}

int gnss_agnss_request_wait(uint32_t timeout_s)
{
	if (active != TRACKER_FIX_SOURCE_MODEM) {
		return -EAGAIN;
	}

	return gnss_modem_agnss_request_wait(timeout_s);
}
