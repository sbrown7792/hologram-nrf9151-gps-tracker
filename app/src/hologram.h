/*
 * Hologram Embedded Cloud Socket API client.
 *
 * Pushes a message into the Hologram Data Engine over a plain TCP socket so the
 * existing REST-API/web-app backend keeps working unchanged.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef GPS_TRACKER_HOLOGRAM_H_
#define GPS_TRACKER_HOLOGRAM_H_

/**
 * @brief Send an already-formatted inner payload string to Hologram.
 *
 * Wraps @p inner_json in the Cloud Socket envelope
 * {"k":<device key>,"d":<inner_json>,"t":[<topic>]} (device key and inner
 * payload are JSON-escaped), connects to the Hologram Cloud Socket, transmits,
 * and checks for the "[0,0]" success reply.
 *
 * In CONFIG_TRACKER_OFFLINE_DEBUG builds the envelope is logged instead of sent.
 *
 * @param inner_json  The telemetry JSON string (becomes the "d" field verbatim).
 * @return 0 on success, negative errno otherwise.
 */
int hologram_send(const char *inner_json);

#endif /* GPS_TRACKER_HOLOGRAM_H_ */
