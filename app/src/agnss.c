/*
 * A-GNSS assistance: download ephemerides, almanac, time and a coarse position
 * from nRF Cloud and inject them into the modem's GNSS.
 *
 * Cuts a cold time-to-first-fix from a minute or more to a few seconds, which
 * on a duty-cycled tracker is mostly a battery saving: the receiver is the
 * expensive part of a wake cycle.
 *
 * Note the request type: over CoAP the encoder only accepts
 * NRF_CLOUD_REST_AGNSS_REQ_CUSTOM and rejects the "just send me everything"
 * types with -ENOTSUP, so a populated request frame is mandatory. It normally
 * comes from the modem itself (NRF_MODEM_GNSS_EVT_AGNSS_REQ, captured in
 * gnss.c); the synthesised fallback exists only for the case where we need
 * assistance before the modem has asked for any.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "agnss.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <modem/lte_lc.h>
#include <modem/modem_info.h>
#include <net/nrf_cloud_agnss.h>
#include <net/nrf_cloud_coap.h>
#include <net/nrf_cloud_rest.h>

#include <stdlib.h>
#include <string.h>

#include "gnss.h"
#include "watchdog.h"

LOG_MODULE_REGISTER(agnss, LOG_LEVEL_INF);

/* Assistance can be several KB; never put this on the caller's stack. */
static uint8_t agnss_buf[NRF_CLOUD_AGNSS_MAX_DATA_SIZE];

/* Uptime of the last successful fetch, or 0 if none yet. */
static int64_t last_fetch_uptime_ms;

/* Fill in the serving cell so the cloud can seed GNSS with a coarse position.
 * Optional: without it assistance still works, just without the location hint.
 */
static int serving_cell_info_get(struct lte_lc_cell *cell)
{
	char resp[MODEM_INFO_MAX_RESPONSE_SIZE];
	int err;

	/* Sentinels, not zeroes: the CoAP encoder tests these exact values to
	 * decide whether to omit the field. A zeroed struct would transmit
	 * "cell 0, rsrp 0", which is wrong data rather than absent data. RSRP
	 * stays invalid - we have it in dBm, the encoder wants the 0..97 index,
	 * and it only refines an already-coarse location hint.
	 */
	cell->id = LTE_LC_CELL_EUTRAN_ID_MAX;
	cell->rsrp = LTE_LC_CELL_RSRP_INVALID;

	err = modem_info_string_get(MODEM_INFO_CELLID, resp, sizeof(resp));
	if (err < 0) {
		return err;
	}
	cell->id = strtol(resp, NULL, 16);

	err = modem_info_string_get(MODEM_INFO_AREA_CODE, resp, sizeof(resp));
	if (err < 0) {
		return err;
	}
	cell->tac = strtol(resp, NULL, 16);

	/* MODEM_INFO_OPERATOR returns MCC and MNC concatenated. */
	err = modem_info_string_get(MODEM_INFO_OPERATOR, resp, sizeof(resp));
	if (err < 0) {
		return err;
	}
	cell->mnc = strtol(&resp[3], NULL, 10);
	resp[3] = '\0';
	cell->mcc = strtol(resp, NULL, 10);

	return 0;
}

/* What to ask for when the modem has not raised an AGNSS_REQ event yet. */
static void synthesize_request(struct nrf_modem_gnss_agnss_data_frame *req)
{
	memset(req, 0, sizeof(*req));

	req->data_flags = NRF_MODEM_GNSS_AGNSS_GPS_UTC_REQUEST |
			  NRF_MODEM_GNSS_AGNSS_KLOBUCHAR_REQUEST |
			  NRF_MODEM_GNSS_AGNSS_NEQUICK_REQUEST |
			  NRF_MODEM_GNSS_AGNSS_GPS_SYS_TIME_AND_SV_TOW_REQUEST |
			  NRF_MODEM_GNSS_AGNSS_POSITION_REQUEST |
			  NRF_MODEM_GNSS_AGNSS_INTEGRITY_REQUEST;
	req->system_count = 1;
	req->system[0].system_id = NRF_MODEM_GNSS_SYSTEM_GPS;
	req->system[0].sv_mask_ephe = 0xFFFFFFFFU;
	req->system[0].sv_mask_alm = 0xFFFFFFFFU;
}

/* Assistance stays useful for hours, so refetching on every failed cycle would
 * just burn data - a covered antenna would otherwise mean a download every wake.
 */
static bool rate_limited(void)
{
	int64_t age_s;

	if (last_fetch_uptime_ms == 0) {
		return false;
	}

	age_s = (k_uptime_get() - last_fetch_uptime_ms) / 1000;
	if (age_s >= CONFIG_TRACKER_AGNSS_MIN_INTERVAL_SECONDS) {
		return false;
	}

	LOG_INF("A-GNSS fetched %lld s ago, skipping (min interval %d s)", age_s,
		CONFIG_TRACKER_AGNSS_MIN_INTERVAL_SECONDS);
	return true;
}

/* Everything from the request build to the modem injection. Split out so the
 * receiver handling and watchdog guard below wrap it exactly once.
 */
static int fetch_and_inject(void)
{
	struct nrf_modem_gnss_agnss_data_frame req;
	struct lte_lc_cells_info net_info = { 0 };
	bool have_cell_info;
	int err;

	if (!gnss_agnss_request_get(&req)) {
		if (!IS_ENABLED(CONFIG_TRACKER_AGNSS_FULL_REQUEST_FALLBACK)) {
			LOG_WRN("GNSS has not requested assistance; nothing to fetch");
			return -EAGAIN;
		}
		LOG_INF("No assistance request from GNSS yet, requesting the full set");
		synthesize_request(&req);
	} else if (req.data_flags != 0) {
		/* If any non-ephemeris item is needed, ask for all of them: the
		 * extra bytes are trivial next to a second round trip.
		 */
		req.data_flags = NRF_MODEM_GNSS_AGNSS_GPS_UTC_REQUEST |
				 NRF_MODEM_GNSS_AGNSS_KLOBUCHAR_REQUEST |
				 NRF_MODEM_GNSS_AGNSS_NEQUICK_REQUEST |
				 NRF_MODEM_GNSS_AGNSS_GPS_SYS_TIME_AND_SV_TOW_REQUEST |
				 NRF_MODEM_GNSS_AGNSS_POSITION_REQUEST |
				 NRF_MODEM_GNSS_AGNSS_INTEGRITY_REQUEST;
	}

	LOG_INF("Requesting A-GNSS: data_flags 0x%02x, ephe mask 0x%08x",
		req.data_flags,
		req.system_count > 0 ? (uint32_t)req.system[0].sv_mask_ephe : 0);

	have_cell_info = (serving_cell_info_get(&net_info.current_cell) == 0);
	if (have_cell_info) {
		net_info.ncells_count = 0;
		net_info.gci_cells_count = 0;
	} else {
		LOG_DBG("No serving cell info; requesting without a location hint");
	}

	struct nrf_cloud_rest_agnss_result result = {
		.buf = agnss_buf,
		.buf_sz = sizeof(agnss_buf),
	};
	struct nrf_cloud_rest_agnss_request request = {
		.type = NRF_CLOUD_REST_AGNSS_REQ_CUSTOM,
		.agnss_req = &req,
		.net_info = have_cell_info ? &net_info : NULL,
		.filtered = IS_ENABLED(CONFIG_NRF_CLOUD_AGNSS_FILTERED),
		.mask_angle = CONFIG_NRF_CLOUD_AGNSS_ELEVATION_MASK,
	};

	err = nrf_cloud_coap_agnss_data_get(&request, &result);
	if (err) {
		LOG_ERR("A-GNSS download failed (err %d)", err);
		return err < 0 ? err : -EIO;
	}

	LOG_INF("A-GNSS data received (%zu bytes), injecting", result.agnss_sz);

	/* Writes straight into the modem via nrf_modem_gnss_agnss_write(). Legal
	 * whether or not the receiver is currently started.
	 */
	err = nrf_cloud_agnss_process(result.buf, result.agnss_sz);
	if (err) {
		LOG_ERR("A-GNSS injection failed (err %d)", err);
		return err;
	}

	last_fetch_uptime_ms = k_uptime_get();
	LOG_INF("A-GNSS data processed");

	return 0;
}

int agnss_fetch_and_inject(void)
{
	bool stopped = IS_ENABLED(CONFIG_TRACKER_AGNSS_STOP_GNSS_DURING_FETCH);
	int err;

	/* Before touching the receiver: stopping and restarting it discards
	 * whatever acquisition progress it has made, which is a bad trade for a
	 * call that is only going to decline.
	 */
	if (rate_limited()) {
		return -EAGAIN;
	}

	/* In LTE-M/GPS coexistence the modem time-shares one radio, so a
	 * searching receiver slows down the very download meant to help it.
	 * Stopping is safe: injection needs GNSS enabled in the functional mode,
	 * not running.
	 */
	if (stopped) {
		gnss_stop();
	}

	/* The main thread blocks inside the CoAP exchange and cannot feed the
	 * watchdog itself, so hand that job to the guard for a bounded window.
	 */
	watchdog_guard_start(CONFIG_TRACKER_AGNSS_BUDGET_SECONDS);
	err = fetch_and_inject();
	watchdog_guard_stop();

	if (stopped && gnss_start() != 0) {
		LOG_ERR("Failed to restart GNSS after the assistance fetch");
		return -EIO;
	}

	return err;
}
