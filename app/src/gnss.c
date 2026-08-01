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
#include <zephyr/spinlock.h>

LOG_MODULE_REGISTER(gnss, LOG_LEVEL_INF);

/* The modem raises GNSS events in interrupt context, so everything shared with
 * the caller is guarded by a spinlock rather than a mutex.
 */
static struct k_spinlock data_lock;
static struct nrf_modem_gnss_pvt_data_frame last_pvt;
static struct nrf_modem_gnss_agnss_data_frame agnss_req;
static bool agnss_req_valid;

static K_SEM_DEFINE(fix_sem, 0, 1);
static K_SEM_DEFINE(agnss_req_sem, 0, 1);

static void gnss_event_handler(int event)
{
	k_spinlock_key_t key;

	switch (event) {
	case NRF_MODEM_GNSS_EVT_PVT: {
		struct nrf_modem_gnss_pvt_data_frame pvt;

		/* Read into a local first: nrf_modem_gnss_read() is a modem RPC and
		 * must not be called while holding the spinlock.
		 */
		if (nrf_modem_gnss_read(&pvt, sizeof(pvt),
					NRF_MODEM_GNSS_DATA_PVT) != 0) {
			return;
		}

		key = k_spin_lock(&data_lock);
		last_pvt = pvt;
		k_spin_unlock(&data_lock, key);

		if (pvt.flags & NRF_MODEM_GNSS_PVT_FLAG_FIX_VALID) {
			k_sem_give(&fix_sem);
		}
		break;
	}
	case NRF_MODEM_GNSS_EVT_AGNSS_REQ: {
		struct nrf_modem_gnss_agnss_data_frame req;

		if (nrf_modem_gnss_read(&req, sizeof(req),
					NRF_MODEM_GNSS_DATA_AGNSS_REQ) != 0) {
			return;
		}

		key = k_spin_lock(&data_lock);
		agnss_req = req;
		agnss_req_valid = true;
		k_spin_unlock(&data_lock, key);

		k_sem_give(&agnss_req_sem);
		break;
	}
	case NRF_MODEM_GNSS_EVT_BLOCKED:
		/* LTE/GNSS coexistence: the modem parked GNSS to service LTE. */
		LOG_DBG("GNSS blocked by LTE");
		break;
	case NRF_MODEM_GNSS_EVT_UNBLOCKED:
		LOG_DBG("GNSS unblocked");
		break;
	default:
		break;
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

#if defined(CONFIG_NRF_CLOUD_AGNSS_FILTERED)
	/* Must match the mask angle sent with the A-GNSS request, otherwise the
	 * modem waits for ephemerides of satellites the cloud filtered out.
	 */
	err = nrf_modem_gnss_elevation_threshold_set(CONFIG_NRF_CLOUD_AGNSS_ELEVATION_MASK);
	if (err) {
		LOG_WRN("Failed to set GNSS elevation threshold (err %d)", err);
	}
#endif

	k_sem_reset(&fix_sem);

	/* Only count assistance requests raised by this run of the receiver. */
	k_sem_reset(&agnss_req_sem);

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

	k_spinlock_key_t key = k_spin_lock(&data_lock);

	*out = last_pvt;
	k_spin_unlock(&data_lock, key);

	LOG_INF("Fix: lat %.06f lon %.06f alt %.01f hdop %.02f acc %.01f m",
		out->latitude, out->longitude, (double)out->altitude,
		(double)out->hdop, (double)out->accuracy);

	return 0;
}

bool gnss_agnss_request_get(struct nrf_modem_gnss_agnss_data_frame *out)
{
	k_spinlock_key_t key = k_spin_lock(&data_lock);
	bool valid = agnss_req_valid;

	if (valid) {
		*out = agnss_req;
	}
	k_spin_unlock(&data_lock, key);

	return valid;
}

int gnss_agnss_request_wait(struct nrf_modem_gnss_agnss_data_frame *out, uint32_t timeout_s)
{
	if (k_sem_take(&agnss_req_sem, K_SECONDS(timeout_s)) != 0) {
		return -EAGAIN;
	}

	return gnss_agnss_request_get(out) ? 0 : -EAGAIN;
}
