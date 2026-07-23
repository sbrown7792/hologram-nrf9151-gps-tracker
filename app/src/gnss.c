/*
 * GNSS fix acquisition using the nRF91 onboard GNSS (nrf_modem_gnss).
 *
 * Continuous-tracking mode (1 Hz). The receiver is started once and kept
 * running; gnss_wait_fix() blocks for the next valid PVT. While externally
 * powered the caller keeps GNSS running between reports so each fix is instant.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "gnss.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gnss, LOG_LEVEL_INF);

static struct nrf_modem_gnss_pvt_data_frame last_pvt;
static K_SEM_DEFINE(fix_sem, 0, 1);

static void gnss_event_handler(int event)
{
	if (event != NRF_MODEM_GNSS_EVT_PVT) {
		return;
	}

	if (nrf_modem_gnss_read(&last_pvt, sizeof(last_pvt),
				NRF_MODEM_GNSS_DATA_PVT) != 0) {
		return;
	}

	if (last_pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
		k_sem_give(&fix_sem);
	}
}

int gnss_init(void)
{
	/* Only the event handler is set here. GNSS parameter configuration must
	 * happen after GNSS is activated (see gnss_start()), otherwise the modem
	 * rejects it with -EACCES.
	 */
	int err = nrf_modem_gnss_event_handler_set(gnss_event_handler);

	if (err) {
		LOG_ERR("Failed to set GNSS event handler (err %d)", err);
		return err;
	}

	return 0;
}

int gnss_start(void)
{
	int err;

	/* Always-recommended use case flag. */
	err = nrf_modem_gnss_use_case_set(NRF_MODEM_GNSS_USE_CASE_MULTIPLE_HOT_START);
	if (err) {
		LOG_WRN("Failed to set GNSS use case (err %d)", err);
	}

	/* Continuous tracking: 1 Hz, no internal retry limit. */
	err = nrf_modem_gnss_fix_retry_set(0);
	if (err) {
		LOG_WRN("Failed to set GNSS fix retry (err %d)", err);
	}

	err = nrf_modem_gnss_fix_interval_set(1);
	if (err) {
		LOG_WRN("Failed to set GNSS fix interval (err %d)", err);
	}

	k_sem_reset(&fix_sem);

	err = nrf_modem_gnss_start();
	if (err) {
		LOG_ERR("Failed to start GNSS (err %d)", err);
		return err;
	}

	LOG_INF("GNSS started (continuous tracking)");
	return 0;
}

void gnss_stop(void)
{
	(void)nrf_modem_gnss_stop();
	LOG_INF("GNSS stopped");
}

int gnss_wait_fix(struct nrf_modem_gnss_pvt_data_frame *out, uint32_t timeout_s)
{
	/* Wait for a fresh valid PVT (the handler signals only on FIX_VALID). */
	k_sem_reset(&fix_sem);

	if (k_sem_take(&fix_sem, K_SECONDS(timeout_s)) != 0) {
		LOG_WRN("GNSS fix timed out after %u s", timeout_s);
		return -EAGAIN;
	}

	*out = last_pvt;

	LOG_INF("Fix: lat %.06f lon %.06f alt %.01f hdop %.02f acc %.01f m",
		out->latitude, out->longitude, (double)out->altitude,
		(double)out->hdop, (double)out->accuracy);

	return 0;
}
