/*
 * Telemetry transport interface.
 *
 * One of the implementations below is compiled in, selected by the
 * TRACKER_CLOUD_TRANSPORT Kconfig choice (see CMakeLists.txt):
 *
 *   cloud_nrf.c       nRF Cloud over CoAP/DTLS  (CONFIG_TRACKER_CLOUD_NRF)
 *   cloud_hologram.c  Hologram Cloud Socket/TCP (CONFIG_TRACKER_CLOUD_HOLOGRAM)
 *
 * main.c talks only to this header, so switching transports never touches the
 * report loop.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_CLOUD_H_
#define GPS_TRACKER_CLOUD_H_

#include <stdbool.h>

#include <nrf_modem_gnss.h>

/**
 * @brief Prepare the transport. Call once at startup.
 *
 * Must run after nrf_modem_lib_init(). Does not need LTE: no network traffic
 * happens here, so a missing credential is reported at boot rather than after
 * the first failed connect.
 *
 * @return 0 on success, negative errno otherwise.
 */
int cloud_init(void);

/**
 * @brief Bring the link up, ready to send.
 *
 * Connects, or cheaply resumes a session paused by cloud_pause(). Safe to call
 * when already connected. Bounded, but can take a while on a cold connect - the
 * caller should hold a watchdog guard across it.
 *
 * @return 0 when ready to send, negative errno otherwise.
 */
int cloud_resume(void);

/**
 * @brief Release the link before a long sleep, keeping session state.
 *
 * Cheaper than disconnecting: the next cloud_resume() can skip the handshake.
 * Never fails in a way the caller can act on.
 */
void cloud_pause(void);

/** @brief Whether a send would currently be attempted over a live link. */
bool cloud_is_ready(void);

/**
 * @brief Send one telemetry report.
 *
 * @param inner_json  The payload from telemetry_build_json(). Transports wrap
 *                    this as needed but never alter it.
 * @return 0 if the report was accepted, negative errno otherwise.
 */
int cloud_send_telemetry(const char *inner_json);

/**
 * @brief Optionally report position in the transport's native format.
 *
 * Used only to make the device visible on a provider's own map view; the
 * telemetry payload above remains the real data path. Compiles to a no-op
 * returning 0 for transports (or configurations) that do not support it.
 *
 * @param pvt  A valid fix. Never called for a synthesised 0,0 report.
 * @return 0 on success or when unsupported, negative errno otherwise.
 */
int cloud_send_location(const struct nrf_modem_gnss_pvt_data_frame *pvt);

#endif /* GPS_TRACKER_CLOUD_H_ */
