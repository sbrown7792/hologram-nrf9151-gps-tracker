/*
 * Telemetry payload builder. See telemetry.h.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "telemetry.h"

#include <errno.h>
#include <stdio.h>

/* Generated at build time by cmake/build_stamp.cmake. */
#include <tracker_build_stamp.h>

/* A point in a battery discharge curve. */
struct battery_level_point {
	uint16_t lvl_pptt; /* remaining capacity, parts-per-ten-thousand */
	uint16_t lvl_mV;   /* voltage at that level */
};

/* Generic single-cell LiPo discharge curve (unloaded-ish), monotonic. */
static const struct battery_level_point lipo_curve[] = {
	{ 10000, 4200 },
	{ 9000, 4050 },
	{ 7500, 3900 },
	{ 5000, 3750 },
	{ 2500, 3600 },
	{ 1000, 3450 },
	{ 0, 3200 },
};

static unsigned int battery_level_pptt(unsigned int batt_mV)
{
	const struct battery_level_point *pb = lipo_curve;

	if (batt_mV >= pb->lvl_mV) {
		return pb->lvl_pptt;
	}

	/* Advance to the first point below the measured voltage. */
	while ((pb->lvl_pptt > 0) && (batt_mV < pb->lvl_mV)) {
		++pb;
	}

	if (batt_mV < pb->lvl_mV) {
		return pb->lvl_pptt; /* below the last point: empty */
	}

	/* Linear interpolation between pb-1 and pb. */
	const struct battery_level_point *pa = pb - 1;

	return pb->lvl_pptt +
	       (pa->lvl_pptt - pb->lvl_pptt) * (batt_mV - pb->lvl_mV) /
		       (pa->lvl_mV - pb->lvl_mV);
}

uint8_t telemetry_battery_percent(uint16_t batt_mv)
{
	return (uint8_t)(battery_level_pptt(batt_mv) / 100U);
}

int telemetry_build_json(const struct tracker_telemetry *t, char *buf, size_t len)
{
	/* fw/hw are build identity rather than measurements, so they come
	 * straight from the build rather than through struct tracker_telemetry -
	 * there is then no way for a caller to forget to fill them in.
	 */
	int n = snprintf(buf, len,
			 "{\"coords\": [%.6f, %.6f], \"hdop\": %.2f, "
			 "\"batt\": %u, \"volt\": %u, \"charge\": %d, "
			 "\"signal\": %d, \"awake\": %u, \"vbus\": %s, "
			 "\"wake\": \"%s\", \"fw\": \"%s\", \"hw\": \"%s\"}",
			 t->longitude, t->latitude, (double)t->hdop,
			 telemetry_battery_percent(t->batt_mv), t->batt_mv,
			 (int)t->charge, t->signal_dbm, t->awake_s,
			 t->vbus ? "true" : "false", wake_reason_name(t->wake),
			 TRACKER_BUILD_STAMP, CONFIG_TRACKER_HW_REVISION);

	if (n < 0 || (size_t)n >= len) {
		return -ENOMEM;
	}

	return n;
}
