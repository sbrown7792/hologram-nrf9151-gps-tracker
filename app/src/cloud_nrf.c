/*
 * nRF Cloud transport over CoAP/DTLS. See cloud.h.
 *
 * Telemetry is wrapped in nRF Cloud's device-message envelope with the payload
 * from telemetry_build_json() embedded as a real JSON object:
 *
 *   {"appId":"<APP_ID>","messageType":"DATA","ts":<ms>,"data":{...}}
 *
 * so the backend unwraps exactly one level. nrf_cloud_coap_json_message_send()
 * posts the string verbatim, which is why the envelope is built here by hand
 * rather than with nrf_cloud_coap_message_send() - that one would escape the
 * payload into a quoted string.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cloud.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <date_time.h>
#include <modem/nrf_modem_lib.h>
#include <net/nrf_cloud.h>
#include <net/nrf_cloud_coap.h>
#include <net/nrf_cloud_defs.h>
#include <nrf_modem_at.h>

#include <errno.h>
#include <stdio.h>

#include "watchdog.h"

LOG_MODULE_REGISTER(cloud_nrf, LOG_LEVEL_INF);

/* Envelope overhead is ~80 bytes; telemetry_build_json() is bounded at 256. */
#define ENVELOPE_MAX	   384
#define MODEM_TIME_POLL_S  3

enum link_state {
	LINK_DISCONNECTED,
	LINK_CONNECTED,
	LINK_PAUSED,
};

static enum link_state link_state;
static bool creds_ok;
static bool modem_time_ready;

/* Positive returns from nrf_cloud_coap_* are CoAP response codes, not errnos. */
static int normalize(const char *what, int err)
{
	if (err > 0) {
		LOG_ERR("%s: server returned CoAP code %d.%02d", what, err / 32, err % 32);
		return -EIO;
	}
	if (err < 0) {
		LOG_ERR("%s failed (err %d)", what, err);
	}

	return err;
}

/* The JWT used to authenticate is signed by the modem, which needs network time
 * first. On a cold boot that can lag registration by a few seconds; without this
 * the first connect of the device's life fails and only the next cycle succeeds.
 */
static void modem_time_wait(void)
{
	char buf[64];

	if (modem_time_ready) {
		return;
	}

	for (int waited = 0; waited < CONFIG_TRACKER_MODEM_TIME_WAIT_SECONDS;
	     waited += MODEM_TIME_POLL_S) {
		if (nrf_modem_at_cmd(buf, sizeof(buf), "AT%%CCLK?") == 0) {
			modem_time_ready = true;
			LOG_INF("Modem network time acquired");
			return;
		}
		k_sleep(K_SECONDS(MODEM_TIME_POLL_S));
		watchdog_feed();
	}

	LOG_WRN("No modem network time after %d s; JWT generation may fail",
		CONFIG_TRACKER_MODEM_TIME_WAIT_SECONDS);
}

static bool credentials_ok(void)
{
	struct nrf_cloud_credentials_status cs;
	int err = nrf_cloud_credentials_check(&cs);

	if (err) {
		LOG_ERR("Credentials check failed (err %d)", err);
		return false;
	}

	/* CoAP needs a CA to verify the server and a private key to sign the JWT. */
	if (!cs.ca || !cs.ca_aws || !cs.prv_key) {
		LOG_ERR("Missing nRF Cloud credentials in sec tag %u:%s%s", cs.sec_tag,
			(!cs.ca || !cs.ca_aws) ? " CA-cert" : "",
			!cs.prv_key ? " private-key" : "");
		LOG_ERR("Provision the device (see README) before it can report");
		return false;
	}

	return true;
}

int cloud_init(void)
{
	char device_id[NRF_CLOUD_CLIENT_ID_MAX_LEN + 1];
	int err;

	if (IS_ENABLED(CONFIG_TRACKER_SKIP_LTE)) {
		LOG_WRN("TRACKER_SKIP_LTE: nRF Cloud disabled");
		return 0;
	}

	err = nrf_cloud_client_id_get(device_id, sizeof(device_id));
	if (err) {
		LOG_ERR("Failed to read the nRF Cloud device ID (err %d)", err);
	} else {
		/* The backend correlates reports by this ID - log it every boot. */
		LOG_INF("nRF Cloud device ID: %s", device_id);
	}

	creds_ok = credentials_ok();

	if (IS_ENABLED(CONFIG_TRACKER_OFFLINE_DEBUG)) {
		return 0;
	}

	if (!creds_ok) {
		return -EACCES;
	}

	err = nrf_cloud_coap_init();
	if (err) {
		LOG_ERR("nrf_cloud_coap_init failed (err %d)", err);
		return err;
	}

	return 0;
}

int cloud_resume(void)
{
	int err;

	if (IS_ENABLED(CONFIG_TRACKER_SKIP_LTE)) {
		return -ENOTCONN;
	}

	if (IS_ENABLED(CONFIG_TRACKER_OFFLINE_DEBUG)) {
		link_state = LINK_CONNECTED;
		return 0;
	}

	/* Latched at init: never burn a doomed handshake once per cycle forever. */
	if (!creds_ok) {
		return -EACCES;
	}

	if (link_state == LINK_CONNECTED) {
		return 0;
	}

	if (link_state == LINK_PAUSED) {
		err = nrf_cloud_coap_resume();
		if (!err) {
			link_state = LINK_CONNECTED;
			LOG_INF("nRF Cloud session resumed (no handshake)");
			return 0;
		}
		LOG_WRN("Session resume failed (err %d), reconnecting", err);
		link_state = LINK_DISCONNECTED;
	}

	modem_time_wait();

	err = nrf_cloud_coap_connect(NULL);
	if (err) {
		LOG_ERR("nRF Cloud connect failed (err %d)", err);
		/* Force a clean handshake next cycle rather than resuming a
		 * half-open session the server may already have discarded.
		 */
		(void)nrf_cloud_coap_disconnect();
		link_state = LINK_DISCONNECTED;
		return err;
	}

	link_state = LINK_CONNECTED;
	LOG_INF("Connected to nRF Cloud");
	return 0;
}

void cloud_pause(void)
{
	if (link_state != LINK_CONNECTED || IS_ENABLED(CONFIG_TRACKER_OFFLINE_DEBUG)) {
		return;
	}

	if (!nrf_cloud_coap_keepopen_is_supported()) {
		/* No DTLS CID to save; leaving the socket open costs nothing while
		 * the modem is in PSM, and the next connect handles the rest.
		 */
		return;
	}

	int err = nrf_cloud_coap_pause();

	if (err) {
		/* -EACCES simply means there was no active CID to save. */
		LOG_DBG("Session pause skipped (err %d)", err);
		return;
	}

	link_state = LINK_PAUSED;
	LOG_DBG("nRF Cloud session paused");
}

bool cloud_is_ready(void)
{
	return link_state == LINK_CONNECTED;
}

int cloud_send_telemetry(const char *inner_json)
{
	char envelope[ENVELOPE_MAX];
	int64_t ts_ms;
	int len;

	/* snprintk(), not snprintf(): CONFIG_NEWLIB_LIBC_NANO is set, so the libc
	 * version cannot be trusted with %lld. No %f is needed here because
	 * telemetry_build_json() has already formatted the floats.
	 */
	if (date_time_now(&ts_ms) == 0) {
		len = snprintk(envelope, sizeof(envelope),
			       "{\"" NRF_CLOUD_JSON_APPID_KEY "\":\"%s\","
			       "\"" NRF_CLOUD_JSON_MSG_TYPE_KEY
			       "\":\"" NRF_CLOUD_JSON_MSG_TYPE_VAL_DATA "\","
			       "\"" NRF_CLOUD_MSG_TIMESTAMP_KEY "\":%lld,"
			       "\"" NRF_CLOUD_JSON_DATA_KEY "\":%s}",
			       CONFIG_TRACKER_NRF_CLOUD_APP_ID, ts_ms, inner_json);
	} else {
		/* No network time yet. Omit "ts" rather than sending a bogus one -
		 * nRF Cloud stamps "receivedAt" server-side regardless.
		 */
		len = snprintk(envelope, sizeof(envelope),
			       "{\"" NRF_CLOUD_JSON_APPID_KEY "\":\"%s\","
			       "\"" NRF_CLOUD_JSON_MSG_TYPE_KEY
			       "\":\"" NRF_CLOUD_JSON_MSG_TYPE_VAL_DATA "\","
			       "\"" NRF_CLOUD_JSON_DATA_KEY "\":%s}",
			       CONFIG_TRACKER_NRF_CLOUD_APP_ID, inner_json);
	}

	if (len < 0 || len >= (int)sizeof(envelope)) {
		LOG_ERR("Device message too large for the envelope buffer");
		return -ENOMEM;
	}

	if (IS_ENABLED(CONFIG_TRACKER_OFFLINE_DEBUG)) {
		LOG_INF("OFFLINE_DEBUG, would send to nRF Cloud:");
		LOG_INF("%s", envelope);
		return 0;
	}

	return normalize("Device message send",
			 nrf_cloud_coap_json_message_send(envelope, false, true));
}

int cloud_send_location(const struct tracker_fix *fix)
{
	if (!IS_ENABLED(CONFIG_TRACKER_NRF_CLOUD_PORTAL_LOCATION)) {
		return 0;
	}

	if (IS_ENABLED(CONFIG_TRACKER_OFFLINE_DEBUG)) {
		LOG_INF("OFFLINE_DEBUG, would send a portal location message");
		return 0;
	}

	/* NRF_CLOUD_GNSS_TYPE_PVT rather than the modem-native MODEM_PVT: it
	 * carries everything the portal map shows and takes the same fields from
	 * either receiver, so there is no per-source encoding here.
	 */
	int64_t ts_ms;
	struct nrf_cloud_gnss_data gnss = {
		.type = NRF_CLOUD_GNSS_TYPE_PVT,
		.ts_ms = (date_time_now(&ts_ms) == 0) ? ts_ms : NRF_CLOUD_NO_TIMESTAMP,
		.pvt = {
			.lat = fix->latitude,
			.lon = fix->longitude,
			.accuracy = fix->accuracy,
			.alt = fix->altitude,
			.speed = fix->speed,
			.heading = fix->heading,
			.has_alt = 1,
			.has_speed = 1,
			.has_heading = 1,
		},
	};

	return normalize("Portal location send", nrf_cloud_coap_location_send(&gnss, false));
}

bool cloud_supports_agnss(void)
{
	return true;
}
