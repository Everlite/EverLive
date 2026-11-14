// SPDX-License-Identifier: GPL-2.0-only
/*
 * gc575_xu.c — AVerMedia GC575 private UVC extension-unit transport
 *
 * The GC575 exposes entity 4 with GUID 961073c7-49f7-44f2-ab42-e940405940c2.
 * Selector 2 carries a little-endian pending length; selector 1 carries AT
 * request/response frames.  This file is the minimal transport shared by the
 * daemon and the gc575ctl diagnostic tool.
 */

#include "gc575_xu.h"

#include <errno.h>
#include <linux/usb/video.h>
#include <linux/uvcvideo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

static int xu_query(int fd, uint8_t unit, uint8_t selector, uint8_t query,
		    void *data, uint16_t size)
{
	struct uvc_xu_control_query q = {
		.unit = unit,
		.selector = selector,
		.query = query,
		.size = size,
		.data = data,
	};

	if (ioctl(fd, UVCIOC_CTRL_QUERY, &q) < 0)
		return -errno;
	return 0;
}

static int xu_get_len(int fd, uint8_t unit, uint8_t selector, uint16_t *len)
{
	uint8_t buf[2] = { 0, 0 };
	int ret = xu_query(fd, unit, selector, UVC_GET_LEN, buf, sizeof(buf));

	if (ret < 0)
		return ret;
	*len = (uint16_t)(buf[0] | (buf[1] << 8));
	return 0;
}

size_t gc575_at_build(uint8_t service, uint32_t command,
		      const uint8_t *payload, size_t payload_len,
		      uint8_t *out, size_t out_size)
{
	size_t body_len = 4 + payload_len;
	size_t n = 0;
	uint8_t sum = 0;

	if (!out || out_size < body_len + 5)
		return 0;

	out[n++] = (uint8_t)((service - 0x60) & 0xff);

	/*
	 * Recovered framing: the short form is used below body length 126;
	 * from 126 upwards the long form avoids the historical length wrap.
	 */
	if (body_len < 126) {
		out[n++] = (uint8_t)((body_len + 2) & 0x7f);
		out[n++] = 0x00;
		out[n++] = 0x00;
	} else {
		out[n++] = (uint8_t)(0x80 | ((body_len + 1) >> 8));
		out[n++] = (uint8_t)((body_len + 1) & 0xff);
		out[n++] = 0x00;
	}

	out[n++] = (uint8_t)(command & 0xff);
	out[n++] = (uint8_t)((command >> 8) & 0xff);
	out[n++] = (uint8_t)((command >> 16) & 0xff);
	out[n++] = (uint8_t)((command >> 24) & 0xff);

	if (payload_len && payload)
		memcpy(out + n, payload, payload_len);
	n += payload_len;

	for (size_t i = 0; i < n; i++)
		sum = (uint8_t)(sum + out[i]);
	out[n++] = (uint8_t)(-sum);

	return n;
}

int gc575_xu_exchange(int fd, uint8_t unit,
		      const uint8_t *frame, size_t frame_len,
		      uint8_t *response, size_t response_size,
		      size_t *response_len)
{
	uint8_t *tx = NULL;
	uint8_t *rx = NULL;
	uint16_t len_size = 2;
	uint16_t data_size = GC575_XU_MAX_PAYLOAD;
	uint16_t pending = 0;
	int ret;

	if (!frame || !response || !response_len || frame_len > 0xffff)
		return -EINVAL;

	ret = xu_get_len(fd, unit, GC575_XU_SEL_LEN, &len_size);
	if (ret < 0)
		return ret;
	ret = xu_get_len(fd, unit, GC575_XU_SEL_DATA, &data_size);
	if (ret < 0)
		return ret;
	if (data_size == 0 || data_size > GC575_XU_MAX_PAYLOAD)
		data_size = GC575_XU_MAX_PAYLOAD;
	if (len_size < 2)
		return -EIO;

	tx = calloc(1, data_size > len_size ? data_size : len_size);
	rx = calloc(1, data_size > len_size ? data_size : len_size);
	if (!tx || !rx) {
		ret = -ENOMEM;
		goto out;
	}

	/* SET_CUR selector 2: outgoing length. */
	tx[0] = (uint8_t)(frame_len & 0xff);
	tx[1] = (uint8_t)((frame_len >> 8) & 0xff);
	ret = xu_query(fd, unit, GC575_XU_SEL_LEN, UVC_SET_CUR, tx, len_size);
	if (ret < 0)
		goto out;

	/* SET_CUR selector 1: AT frame, zero-padded to the control size. */
	memset(tx, 0, data_size);
	if (frame_len > data_size) {
		ret = -E2BIG;
		goto out;
	}
	memcpy(tx, frame, frame_len);
	ret = xu_query(fd, unit, GC575_XU_SEL_DATA, UVC_SET_CUR, tx, data_size);
	if (ret < 0)
		goto out;

	/* GET_CUR selector 2: pending response length. */
	memset(rx, 0, data_size);
	ret = xu_query(fd, unit, GC575_XU_SEL_LEN, UVC_GET_CUR, rx, len_size);
	if (ret < 0)
		goto out;
	pending = (uint16_t)(rx[0] | (rx[1] << 8));

	/* GET_CUR selector 1: response bytes. */
	memset(rx, 0, data_size);
	ret = xu_query(fd, unit, GC575_XU_SEL_DATA, UVC_GET_CUR, rx, data_size);
	if (ret < 0)
		goto out;

	size_t actual = pending;
	if (actual == 0 || actual > data_size) {
		/* Derive the frame length from the recovered AT envelope. */
		uint8_t h0 = rx[0];
		uint8_t h1 = rx[1];
		if (h0 & 0x80)
			actual = (size_t)(((h0 & 0x7f) << 8) | h1) + 4;
		else if (h0 >= 2)
			actual = (size_t)h0 + 3;
		else
			actual = data_size;
	}
	if (actual > data_size)
		actual = data_size;
	if (actual > response_size)
		actual = response_size;

	memcpy(response, rx, actual);
	*response_len = actual;
	ret = 0;

out:
	free(tx);
	free(rx);
	return ret;
}

int gc575_xu_set_profile(int fd, uint8_t unit, uint32_t index)
{
	uint8_t payload[8] = {
		0x01, 0x00, 0x00, 0x00,
		(uint8_t)(index & 0xff),
		(uint8_t)((index >> 8) & 0xff),
		(uint8_t)((index >> 16) & 0xff),
		(uint8_t)((index >> 24) & 0xff),
	};
	uint8_t frame[32];
	uint8_t response[GC575_XU_MAX_PAYLOAD];
	size_t frame_len;
	size_t response_len = 0;

	frame_len = gc575_at_build(1, 0xb0, payload, sizeof(payload),
				   frame, sizeof(frame));
	if (!frame_len)
		return -EINVAL;

	return gc575_xu_exchange(fd, unit, frame, frame_len,
				 response, sizeof(response), &response_len);
}

int gc575_xu_get_profile(int fd, uint8_t unit, uint32_t *index)
{
	uint8_t frame[16];
	uint8_t response[GC575_XU_MAX_PAYLOAD];
	size_t frame_len;
	size_t response_len = 0;
	int ret;

	if (!index)
		return -EINVAL;

	frame_len = gc575_at_build(1, 0xb1, NULL, 0, frame, sizeof(frame));
	if (!frame_len)
		return -EINVAL;

	ret = gc575_xu_exchange(fd, unit, frame, frame_len,
				response, sizeof(response), &response_len);
	if (ret < 0)
		return ret;
	if (response_len < 9)
		return -EIO;

	/* SDK result byte at offset 4 of the response payload. */
	*index = response[4 + 4];
	return 0;
}

int gc575_xu_get_info(int fd, uint8_t unit, char *out, size_t out_size)
{
	uint8_t frame[16];
	uint8_t response[GC575_XU_MAX_PAYLOAD];
	size_t frame_len;
	size_t response_len = 0;
	size_t body_len;
	int ret;

	if (!out || out_size == 0)
		return -EINVAL;

	frame_len = gc575_at_build(1, 0x23, NULL, 0, frame, sizeof(frame));
	if (!frame_len)
		return -EINVAL;

	ret = gc575_xu_exchange(fd, unit, frame, frame_len,
				response, sizeof(response), &response_len);
	if (ret < 0)
		return ret;
	if (response_len < 6)
		return -EIO;

	body_len = response_len - 5; /* 4-byte envelope + checksum */
	if (body_len >= out_size)
		body_len = out_size - 1;
	memcpy(out, response + 4, body_len);
	out[body_len] = '\0';
	return 0;
}
