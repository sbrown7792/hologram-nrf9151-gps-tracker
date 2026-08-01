/*
 * Hologram Cloud Socket transport. See cloud.h.
 *
 * A thin adapter over hologram.c: the Cloud Socket API opens a fresh TCP
 * connection per message, so there is no session to bring up, pause or resume.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "cloud.h"

#include <zephyr/kernel.h>

#include "hologram.h"

int cloud_init(void)
{
	return 0;
}

int cloud_resume(void)
{
	return 0;
}

void cloud_pause(void)
{
}

bool cloud_is_ready(void)
{
	return true;
}

int cloud_send_telemetry(const char *inner_json)
{
	return hologram_send(inner_json);
}

int cloud_send_location(const struct nrf_modem_gnss_pvt_data_frame *pvt)
{
	/* The Data Engine has no native position format; coords are already in
	 * the telemetry payload.
	 */
	ARG_UNUSED(pvt);
	return 0;
}
