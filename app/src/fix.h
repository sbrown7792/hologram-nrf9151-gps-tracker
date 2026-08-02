/*
 * A position fix, independent of which receiver produced it.
 *
 * The tracker can take its position either from the nRF9151's onboard GNSS
 * (nrf_modem_gnss, a struct nrf_modem_gnss_pvt_data_frame) or from an external
 * NMEA module on UART (a struct gnss_data from Zephyr's GNSS subsystem). Both
 * are normalised into this so nothing downstream - telemetry, the report loop,
 * the cloud transports - has to care.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_FIX_H_
#define GPS_TRACKER_FIX_H_

#include <stdint.h>

enum tracker_fix_source {
	TRACKER_FIX_SOURCE_MODEM,
	TRACKER_FIX_SOURCE_EXTERNAL,
};

struct tracker_fix {
	double latitude;	/* degrees */
	double longitude;	/* degrees */
	float altitude;		/* metres above the WGS-84 ellipsoid */
	float accuracy;		/* 2D 1-sigma position accuracy, metres */
	float speed;		/* horizontal speed, m/s */
	float heading;		/* direction of movement, degrees from true north */
	float hdop;		/* horizontal dilution of precision */
	uint16_t satellites;	/* satellites used in the solution, 0 if unknown */
	enum tracker_fix_source source;
};

/** @brief Human-readable name of a fix source, for logging. */
static inline const char *tracker_fix_source_name(enum tracker_fix_source source)
{
	return source == TRACKER_FIX_SOURCE_EXTERNAL ? "external" : "modem";
}

#endif /* GPS_TRACKER_FIX_H_ */
