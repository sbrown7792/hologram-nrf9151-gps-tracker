/*
 * Telemetry payload builder.
 *
 * Produces the *inner* JSON string that the web-app/backend already expects,
 * byte-compatible with the original Hologram Dash sketch:
 *
 *   {"coords": [<lon>, <lat>], "hdop": <h>, "batt": <pct>, "volt": <mV>,
 *    "charge": <state>, "signal": <rsrp_dbm>, "awake": <secs>}
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_TELEMETRY_H_
#define GPS_TRACKER_TELEMETRY_H_

#include <stddef.h>
#include <stdint.h>

#include "power.h"

struct tracker_telemetry {
	double longitude;
	double latitude;
	float hdop;
	uint16_t batt_mv;
	enum tracker_charge_state charge;
	int signal_dbm; /* RSRP in dBm (negative); replaces the Dash CSQ value */
	uint32_t awake_s;
};

/**
 * @brief Estimate battery percentage (0-100) from voltage using a LiPo curve.
 */
uint8_t telemetry_battery_percent(uint16_t batt_mv);

/**
 * @brief Build the inner telemetry JSON string.
 *
 * @param t     Telemetry values.
 * @param buf   Output buffer.
 * @param len   Size of @p buf.
 * @return number of bytes written (excluding NUL), or negative on truncation.
 */
int telemetry_build_json(const struct tracker_telemetry *t, char *buf, size_t len);

#endif /* GPS_TRACKER_TELEMETRY_H_ */
