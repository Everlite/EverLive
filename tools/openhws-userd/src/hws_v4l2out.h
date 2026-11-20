/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * hws_v4l2out.h — v4l2loopback writer for the HWS UHD engine.
 */
#ifndef HWS_V4L2OUT_H
#define HWS_V4L2OUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct hws_v4l2out {
	int fd;
	char path[64];
	bool configured;
	unsigned int src_w, src_h;	/* frames handed in */
	unsigned int out_w, out_h;	/* negotiated node format */
	uint64_t last_try_ms;
	uint8_t *scaled;
	size_t scaled_size;
	uint64_t frames;
	uint64_t dropped;
};

int hws_v4l2out_open(struct hws_v4l2out *o, const char *path);
void hws_v4l2out_close(struct hws_v4l2out *o);
/* Write one YUYV frame; negotiates or rescales as needed. */
int hws_v4l2out_write(struct hws_v4l2out *o, const uint8_t *frame, unsigned int w, unsigned int h);

#endif /* HWS_V4L2OUT_H */
