/*
 * Hologram Embedded Cloud Socket API client. See hologram.h.
 *
 * Protocol (docs.hologram.io "Embedded APIs"): open a TCP connection to
 * cloudsocket.hologram.io:9999 and send
 *   {"k":"<8-char device key>","d":"<data>","t":["<topic>"]}
 * followed by two newlines. On success the server returns "[0,0]".
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "hologram.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/socket.h>

#include <cJSON.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(hologram, LOG_LEVEL_INF);

#define ENVELOPE_MAX 512
#define REPLY_MAX    32

/* Build {"k":...,"d":...,"t":[...]} + "\n\n" into buf. */
static int build_envelope(const char *inner_json, char *buf, size_t len)
{
	cJSON *root = cJSON_CreateObject();

	if (root == NULL) {
		return -ENOMEM;
	}

	cJSON *topics = cJSON_CreateArray();

	if (topics == NULL ||
	    cJSON_AddStringToObject(root, "k", CONFIG_HOLOGRAM_DEVICE_KEY) == NULL ||
	    cJSON_AddStringToObject(root, "d", inner_json) == NULL) {
		cJSON_Delete(topics);
		cJSON_Delete(root);
		return -ENOMEM;
	}

	cJSON_AddItemToArray(topics, cJSON_CreateString(CONFIG_HOLOGRAM_TOPIC));
	cJSON_AddItemToObject(root, "t", topics);

	int ret = -ENOMEM;

	if (cJSON_PrintPreallocated(root, buf, len - 3, false)) {
		/* Append the required double-newline terminator. */
		strcat(buf, "\n\n");
		ret = (int)strlen(buf);
	}

	cJSON_Delete(root);
	return ret;
}

static int socket_send_envelope(const char *envelope, size_t envelope_len)
{
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *res = NULL;
	int fd = -1;
	int err;

	err = getaddrinfo(CONFIG_HOLOGRAM_HOST, NULL, &hints, &res);
	if (err != 0 || res == NULL) {
		LOG_ERR("getaddrinfo(%s) failed (err %d)", CONFIG_HOLOGRAM_HOST, err);
		return -EIO;
	}

	((struct sockaddr_in *)res->ai_addr)->sin_port = htons(CONFIG_HOLOGRAM_PORT);

	fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd < 0) {
		LOG_ERR("socket() failed (err %d)", -errno);
		err = -errno;
		goto cleanup;
	}

	err = connect(fd, res->ai_addr, res->ai_addrlen);
	if (err != 0) {
		LOG_ERR("connect() failed (err %d)", -errno);
		err = -errno;
		goto cleanup;
	}

#if defined(CONFIG_LTE_LC_RAI_MODULE) && defined(SO_RAI)
	/* Release Assistance: this is the last data we will send. */
	int rai = RAI_LAST;

	if (setsockopt(fd, SOL_SOCKET, SO_RAI, &rai, sizeof(rai)) != 0) {
		LOG_DBG("SO_RAI not applied (err %d)", -errno);
	}
#endif

	ssize_t sent = send(fd, envelope, envelope_len, 0);

	if (sent < 0) {
		LOG_ERR("send() failed (err %d)", -errno);
		err = -errno;
		goto cleanup;
	}

	/* Read the "[0,0]" acknowledgement. */
	char reply[REPLY_MAX] = {0};
	ssize_t rx = recv(fd, reply, sizeof(reply) - 1, 0);

	if (rx > 0 && strstr(reply, "[0,0]") != NULL) {
		LOG_INF("Hologram accepted message (%s)", reply);
		err = 0;
	} else {
		LOG_ERR("Hologram send not acknowledged (rx %d: %s)", (int)rx, reply);
		err = -EIO;
	}

cleanup:
	if (fd >= 0) {
		close(fd);
	}
	freeaddrinfo(res);
	return err;
}

int hologram_send(const char *inner_json)
{
	char envelope[ENVELOPE_MAX];
	int len = build_envelope(inner_json, envelope, sizeof(envelope));

	if (len < 0) {
		LOG_ERR("Failed to build Hologram envelope");
		return len;
	}

	if (IS_ENABLED(CONFIG_TRACKER_OFFLINE_DEBUG)) {
		LOG_INF("OFFLINE_DEBUG, would send to %s:%d:",
			CONFIG_HOLOGRAM_HOST, CONFIG_HOLOGRAM_PORT);
		LOG_INF("%s", envelope);
		return 0;
	}

	return socket_send_envelope(envelope, (size_t)len);
}
