/*
 * A-GNSS assistance fetch. See agnss.c.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_AGNSS_H_
#define GPS_TRACKER_AGNSS_H_

#include <errno.h>

#if defined(CONFIG_TRACKER_AGNSS)

/**
 * @brief Download assistance data from nRF Cloud and inject it into the modem.
 *
 * Requires a live cloud link (see cloud_resume()). GNSS may be running or
 * stopped - injection only needs GNSS enabled in the current functional mode.
 * Rate-limited internally, so it is safe to call on every failed fix.
 *
 * @return 0 if assistance was injected, -EAGAIN if rate-limited, other negative
 *         errno on failure.
 */
int agnss_fetch_and_inject(void);

#else /* CONFIG_TRACKER_AGNSS */

static inline int agnss_fetch_and_inject(void)
{
	return -ENOTSUP;
}

#endif /* CONFIG_TRACKER_AGNSS */

#endif /* GPS_TRACKER_AGNSS_H_ */
