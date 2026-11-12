/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_nosignal.c — Procedural YUYV standby screen for EverLive
 *
 * Draws a dark-gaming "NO HDMI SIGNAL" card into an RGB scratch canvas and
 * converts it once to BT.601 limited-range YUYV, which is what the SDL2
 * YUY2 texture and the v4l2loopback sink both expect.  Nothing is
 * regenerated while the geometry stays the same, so pushing the frame at
 * 30 fps costs one memcpy to the GPU and one write() to the loopback node.
 */

#include "gc573_user_nosignal.h"
#include "gc573_user_font8x8.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define NS_MAX_WIDTH	7680U
#define NS_MAX_HEIGHT	4320U

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static float ns_clampf(float v, float lo, float hi)
{
	if (v < lo)
		return lo;
	if (v > hi)
		return hi;
	return v;
}

static void ns_blend_at(uint8_t *rgb, unsigned int w, unsigned int h,
			int x, int y, int r, int g, int b, float alpha)
{
	uint8_t *p;

	if (alpha <= 0.0f)
		return;
	if (x < 0 || y < 0 || (unsigned int)x >= w || (unsigned int)y >= h)
		return;

	alpha = ns_clampf(alpha, 0.0f, 1.0f);
	p = rgb + ((size_t)y * w + (unsigned int)x) * 3U;
	p[0] = (uint8_t)((float)p[0] + ((float)r - (float)p[0]) * alpha);
	p[1] = (uint8_t)((float)p[1] + ((float)g - (float)p[1]) * alpha);
	p[2] = (uint8_t)((float)p[2] + ((float)b - (float)p[2]) * alpha);
}

static void ns_fill_rect(uint8_t *rgb, unsigned int w, unsigned int h,
			 int x, int y, int rw, int rh,
			 int r, int g, int b, float alpha)
{
	for (int yy = y; yy < y + rh; yy++)
		for (int xx = x; xx < x + rw; xx++)
			ns_blend_at(rgb, w, h, xx, yy, r, g, b, alpha);
}

/*
 * Signed distance to a rounded box centred at the origin.  hw/hh are the
 * half extents of the *unrounded* box, radius is the corner radius.
 */
static float ns_round_box_dist(float px, float py, float hw, float hh,
			       float radius)
{
	float qx = fabsf(px) - hw + radius;
	float qy = fabsf(py) - hh + radius;
	float ax = qx > 0.0f ? qx : 0.0f;
	float ay = qy > 0.0f ? qy : 0.0f;
	float outside = sqrtf(ax * ax + ay * ay);
	float inside = fminf(fmaxf(qx, qy), 0.0f);

	return outside + inside - radius;
}

static void ns_fill_round_box(uint8_t *rgb, unsigned int w, unsigned int h,
			      float cx, float cy, float hw, float hh,
			      float radius, int r, int g, int b, float alpha)
{
	int x0 = (int)floorf(cx - hw) - 1;
	int y0 = (int)floorf(cy - hh) - 1;
	int x1 = (int)ceilf(cx + hw) + 1;
	int y1 = (int)ceilf(cy + hh) + 1;

	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			float d = ns_round_box_dist((float)x + 0.5f - cx,
						    (float)y + 0.5f - cy,
						    hw, hh, radius);

			ns_blend_at(rgb, w, h, x, y, r, g, b,
				    ns_clampf(0.5f - d, 0.0f, 1.0f) * alpha);
		}
	}
}

static void ns_stroke_round_box(uint8_t *rgb, unsigned int w, unsigned int h,
				float cx, float cy, float hw, float hh,
				float radius, float thickness,
				int r, int g, int b, float alpha)
{
	float half = thickness * 0.5f;
	int x0 = (int)floorf(cx - hw - half) - 2;
	int y0 = (int)floorf(cy - hh - half) - 2;
	int x1 = (int)ceilf(cx + hw + half) + 2;
	int y1 = (int)ceilf(cy + hh + half) + 2;

	for (int y = y0; y <= y1; y++) {
		for (int x = x0; x <= x1; x++) {
			float d = ns_round_box_dist((float)x + 0.5f - cx,
						    (float)y + 0.5f - cy,
						    hw, hh, radius);
			float coverage = ns_clampf(half + 0.5f - fabsf(d),
						   0.0f, 1.0f);

			ns_blend_at(rgb, w, h, x, y, r, g, b, coverage * alpha);
		}
	}
}

static void ns_draw_segment(uint8_t *rgb, unsigned int w, unsigned int h,
			    float x0, float y0, float x1, float y1,
			    float thickness, int r, int g, int b, float alpha)
{
	float dx = x1 - x0;
	float dy = y1 - y0;
	float len2 = dx * dx + dy * dy;
	float half = thickness * 0.5f;
	int bx0 = (int)floorf(fminf(x0, x1) - half) - 2;
	int by0 = (int)floorf(fminf(y0, y1) - half) - 2;
	int bx1 = (int)ceilf(fmaxf(x0, x1) + half) + 2;
	int by1 = (int)ceilf(fmaxf(y0, y1) + half) + 2;

	if (len2 <= 0.0f)
		return;

	for (int y = by0; y <= by1; y++) {
		for (int x = bx0; x <= bx1; x++) {
			float px = (float)x + 0.5f - x0;
			float py = (float)y + 0.5f - y0;
			float t = (px * dx + py * dy) / len2;
			float ex, ey, dist;

			t = ns_clampf(t, 0.0f, 1.0f);
			ex = px - dx * t;
			ey = py - dy * t;
			dist = sqrtf(ex * ex + ey * ey);
			ns_blend_at(rgb, w, h, x, y, r, g, b,
				    ns_clampf(half + 0.5f - dist, 0.0f, 1.0f) *
					    alpha);
		}
	}
}

static int ns_text_width(const char *text, int scale, int tracking)
{
	int len = (int)strlen(text);

	if (len <= 0)
		return 0;
	return len * 8 * scale + (len - 1) * tracking;
}

static void ns_draw_text(uint8_t *rgb, unsigned int w, unsigned int h,
			 float x, float y, int scale, int tracking,
			 const char *text, int r, int g, int b, float alpha)
{
	int pen_x = (int)lroundf(x);
	int pen_y = (int)lroundf(y);

	for (const char *p = text; *p; p++) {
		unsigned char glyph = (unsigned char)*p;
		const uint8_t *bitmap;

		if (glyph > 127)
			glyph = '?';
		bitmap = font8x8_basic[glyph];

		for (int row = 0; row < 8; row++) {
			uint8_t bits = bitmap[row];

			if (!bits)
				continue;
			for (int col = 0; col < 8; col++) {
				if (!(bits & (1 << col)))
					continue;
				ns_fill_rect(rgb, w, h,
					     pen_x + col * scale,
					     pen_y + row * scale,
					     scale, scale, r, g, b, alpha);
			}
		}
		pen_x += 8 * scale + tracking;
	}
}

static void ns_draw_text_shadow(uint8_t *rgb, unsigned int w, unsigned int h,
				float x, float y, int scale, int tracking,
				const char *text, float offset, float alpha)
{
	ns_draw_text(rgb, w, h, x + offset, y + offset, scale, tracking,
		     text, 0, 0, 0, alpha);
}

/* ------------------------------------------------------------------ */
/* Background                                                         */
/* ------------------------------------------------------------------ */

static void ns_draw_background(uint8_t *rgb, unsigned int w, unsigned int h)
{
	const float cx = (float)w * 0.5f;
	const float cy = (float)h * 0.42f;
	const float inv_w = 1.0f / (float)(w > 1U ? w - 1U : 1U);
	const float inv_h = 1.0f / (float)(h > 1U ? h - 1U : 1U);

	for (unsigned int y = 0; y < h; y++) {
		float ty = (float)y * inv_h;
		uint8_t *row = rgb + (size_t)y * w * 3U;

		for (unsigned int x = 0; x < w; x++) {
			float tx = (float)x * inv_w;
			float m = 0.72f * ty + 0.28f * (1.0f - tx);
			float r = 7.0f + 9.0f * m;
			float g = 11.0f + 13.0f * m;
			float b = 20.0f + 20.0f * m;
			float gx = ((float)x - cx) / (0.42f * (float)w);
			float gy = ((float)y - cy) / (0.55f * (float)h);
			float d2 = gx * gx + gy * gy;
			float vx = ((float)x - cx) / (0.62f * (float)w);
			float vy = ((float)y - (float)h * 0.5f) /
				   (0.62f * (float)h);
			float vignette = 1.0f -
					 0.22f * fminf(1.0f, vx * vx + vy * vy);
			uint8_t *p = row + (size_t)x * 3U;

			/* Soft cyan halo behind the icon. */
			if (d2 < 1.0f) {
				float k = 1.0f - d2;
				float a = 0.07f * k * k;

				r += (30.0f - r) * a;
				g += (120.0f - g) * a;
				b += (150.0f - b) * a;
			}

			p[0] = (uint8_t)ns_clampf(r * vignette, 0.0f, 255.0f);
			p[1] = (uint8_t)ns_clampf(g * vignette, 0.0f, 255.0f);
			p[2] = (uint8_t)ns_clampf(b * vignette, 0.0f, 255.0f);
		}
	}
}

static void ns_draw_grid(uint8_t *rgb, unsigned int w, unsigned int h)
{
	float s = (float)h / 1080.0f;
	unsigned int step = (unsigned int)lroundf(80.0f * s);

	if (step < 24U)
		step = 24U;

	for (unsigned int y = 0, row = 0; y < h; y += step, row++) {
		float alpha = (row % 3U == 0U) ? 0.075f : 0.035f;

		for (unsigned int x = 0; x < w; x++)
			ns_blend_at(rgb, w, h, (int)x, (int)y,
				    148, 163, 184, alpha);
	}
	for (unsigned int x = 0, col = 0; x < w; x += step, col++) {
		float alpha = (col % 3U == 0U) ? 0.075f : 0.035f;

		for (unsigned int y = 0; y < h; y++)
			ns_blend_at(rgb, w, h, (int)x, (int)y,
				    148, 163, 184, alpha);
	}
}

/* ------------------------------------------------------------------ */
/* Foreground                                                         */
/* ------------------------------------------------------------------ */

static void ns_draw_layout(uint8_t *rgb, unsigned int w, unsigned int h)
{
	const float s = (float)h / 1080.0f;
	const float cx = (float)w * 0.5f;
	float icon_w, icon_h, icon_cy, radius, stroke;
	float bar_w, bar_h, bar_y, mark_y;
	int mark_scale, mark_track, mark_w;
	int title_scale, title_track, title_w, sub_scale, sub_track, sub_w;
	float title_y, sub_y;
	const char *title = "NO HDMI SIGNAL";
	const char *subtitle = "Waiting for console / input...";
	const char *footer = "EVERLIVE  |  USERSPACE VFIO ENGINE";

	/* ---- wordmark ------------------------------------------------- */
	mark_scale = (int)lroundf(22.0f * s / 8.0f);
	if (mark_scale < 1)
		mark_scale = 1;
	mark_track = (int)lroundf(4.0f * s);
	mark_w = ns_text_width("EVERLIVE", mark_scale, mark_track);
	mark_y = 0.20f * (float)h;
	ns_draw_text(rgb, w, h, cx - (float)mark_w * 0.5f, mark_y,
		     mark_scale, mark_track, "EVERLIVE", 34, 211, 238, 0.92f);

	bar_w = 64.0f * s;
	bar_h = fmaxf(2.0f, 3.0f * s);
	bar_y = mark_y + (float)(8 * mark_scale) + 18.0f * s;
	ns_fill_rect(rgb, w, h,
		     (int)lroundf(cx - bar_w * 0.5f), (int)lroundf(bar_y),
		     (int)lroundf(bar_w), (int)lroundf(bar_h),
		     34, 211, 238, 0.55f);

	/* ---- monitor icon --------------------------------------------- */
	icon_w = 520.0f * s;
	if (icon_w > 0.60f * (float)w)
		icon_w = 0.60f * (float)w;
	icon_h = icon_w * 0.58f;
	radius = fmaxf(6.0f, 18.0f * s);
	stroke = fmaxf(3.0f, 6.0f * s);
	icon_cy = bar_y + bar_h + 42.0f * s + icon_h * 0.5f;

	ns_fill_round_box(rgb, w, h, cx, icon_cy, icon_w * 0.5f,
			  icon_h * 0.5f, radius, 10, 17, 28, 0.60f);
	ns_stroke_round_box(rgb, w, h, cx, icon_cy, icon_w * 0.5f,
			    icon_h * 0.5f, radius, stroke, 56, 189, 248, 0.85f);

	/* stand */
	ns_draw_segment(rgb, w, h, cx, icon_cy + icon_h * 0.5f,
			cx, icon_cy + icon_h * 0.5f + 26.0f * s,
			stroke * 0.9f, 56, 189, 248, 0.75f);
	ns_draw_segment(rgb, w, h,
			cx - icon_w * 0.16f,
			icon_cy + icon_h * 0.5f + 26.0f * s,
			cx + icon_w * 0.16f,
			icon_cy + icon_h * 0.5f + 26.0f * s,
			stroke, 56, 189, 248, 0.75f);

	/* slash: dark underlay first so the red reads on any background */
	ns_draw_segment(rgb, w, h,
			cx - icon_w * 0.40f, icon_cy - icon_h * 0.33f,
			cx + icon_w * 0.40f, icon_cy + icon_h * 0.33f,
			stroke * 2.4f, 8, 11, 18, 0.70f);
	ns_draw_segment(rgb, w, h,
			cx - icon_w * 0.40f, icon_cy - icon_h * 0.33f,
			cx + icon_w * 0.40f, icon_cy + icon_h * 0.33f,
			stroke * 1.35f, 248, 113, 113, 0.95f);

	/* ---- headline ------------------------------------------------- */
	title_scale = (int)lroundf(72.0f * s / 8.0f);
	if (title_scale < 1)
		title_scale = 1;
	title_track = (int)lroundf(2.0f * s);
	title_w = ns_text_width(title, title_scale, title_track);
	title_y = icon_cy + icon_h * 0.5f + 26.0f * s + 44.0f * s;

	ns_draw_text_shadow(rgb, w, h, cx - (float)title_w * 0.5f, title_y,
			    title_scale, title_track, title,
			    fmaxf(2.0f, 3.0f * s), 0.55f);
	ns_draw_text(rgb, w, h, cx - (float)title_w * 0.5f, title_y,
		     title_scale, title_track, title, 226, 232, 240, 1.0f);

	/* ---- subtitle ------------------------------------------------- */
	sub_scale = (int)lroundf(26.0f * s / 8.0f);
	if (sub_scale < 1)
		sub_scale = 1;
	sub_track = (int)lroundf(1.0f * s);
	sub_w = ns_text_width(subtitle, sub_scale, sub_track);
	sub_y = title_y + (float)(8 * title_scale) + 20.0f * s;
	ns_draw_text(rgb, w, h, cx - (float)sub_w * 0.5f, sub_y,
		     sub_scale, sub_track, subtitle, 100, 116, 139, 1.0f);

	/* ---- footer --------------------------------------------------- */
	{
		int foot_scale = 2;
		int foot_track = (int)lroundf(2.0f * s);
		int foot_w = ns_text_width(footer, foot_scale, foot_track);
		float foot_y = (float)h - 46.0f * s;

		ns_draw_text(rgb, w, h, cx - (float)foot_w * 0.5f, foot_y,
			     foot_scale, foot_track, footer, 51, 65, 85, 0.9f);
	}
}

/* ------------------------------------------------------------------ */
/* Colour conversion                                                  */
/* ------------------------------------------------------------------ */

static uint8_t ns_y_bt601(int r, int g, int b)
{
	int v = 16 + (66 * r + 129 * g + 25 * b + 128) / 256;

	return (uint8_t)(v < 16 ? 16 : (v > 235 ? 235 : v));
}

static uint8_t ns_u_bt601(int r, int g, int b)
{
	int v = 128 + (-38 * r - 74 * g + 112 * b + 128) / 256;

	return (uint8_t)(v < 16 ? 16 : (v > 240 ? 240 : v));
}

static uint8_t ns_v_bt601(int r, int g, int b)
{
	int v = 128 + (112 * r - 94 * g - 18 * b + 128) / 256;

	return (uint8_t)(v < 16 ? 16 : (v > 240 ? 240 : v));
}

static void ns_rgb_to_yuyv(const uint8_t *rgb, uint8_t *yuyv,
			   unsigned int w, unsigned int h)
{
	for (unsigned int y = 0; y < h; y++) {
		const uint8_t *src = rgb + (size_t)y * w * 3U;
		uint8_t *dst = yuyv + (size_t)y * w * 2U;

		for (unsigned int x = 0; x < w; x += 2U) {
			const uint8_t *p0 = src + (size_t)x * 3U;
			const uint8_t *p1 = p0 + 3U;
			int r = (p0[0] + p1[0] + 1) / 2;
			int g = (p0[1] + p1[1] + 1) / 2;
			int b = (p0[2] + p1[2] + 1) / 2;

			dst[x * 2U + 0U] = ns_y_bt601(p0[0], p0[1], p0[2]);
			dst[x * 2U + 1U] = ns_u_bt601(r, g, b);
			dst[x * 2U + 2U] = ns_y_bt601(p1[0], p1[1], p1[2]);
			dst[x * 2U + 3U] = ns_v_bt601(r, g, b);
		}
	}
}

static int ns_generate(uint8_t *yuyv, unsigned int w, unsigned int h)
{
	uint8_t *rgb = malloc((size_t)w * h * 3U);

	if (!rgb)
		return -ENOMEM;

	ns_draw_background(rgb, w, h);
	ns_draw_grid(rgb, w, h);
	ns_draw_layout(rgb, w, h);
	ns_rgb_to_yuyv(rgb, yuyv, w, h);
	free(rgb);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

int gc573_no_signal_ensure(struct gc573_no_signal *ns, unsigned int width,
			   unsigned int height)
{
	uint8_t *pixels;
	int ret;

	if (!ns)
		return -EINVAL;
	if (width < 16U || height < 16U || (width & 1U) ||
	    width > NS_MAX_WIDTH || height > NS_MAX_HEIGHT)
		return -EINVAL;

	pthread_mutex_lock(&ns->lock);
	if (ns->pixels && ns->width == width && ns->height == height) {
		pthread_mutex_unlock(&ns->lock);
		return 0;
	}

	pixels = malloc((size_t)width * height * 2U);
	if (!pixels) {
		pthread_mutex_unlock(&ns->lock);
		return -ENOMEM;
	}

	ret = ns_generate(pixels, width, height);
	if (ret < 0) {
		free(pixels);
		pthread_mutex_unlock(&ns->lock);
		return ret;
	}

	free(ns->pixels);
	ns->pixels = pixels;
	ns->width = width;
	ns->height = height;
	ns->bytes = (size_t)width * height * 2U;
	ns->generation++;
	pthread_mutex_unlock(&ns->lock);
	return 0;
}

const void *gc573_no_signal_frame(struct gc573_no_signal *ns, size_t *bytes)
{
	const void *frame;

	if (!ns)
		return NULL;

	pthread_mutex_lock(&ns->lock);
	frame = ns->pixels;
	if (bytes)
		*bytes = ns->bytes;
	pthread_mutex_unlock(&ns->lock);
	return frame;
}

unsigned int gc573_no_signal_width(const struct gc573_no_signal *ns)
{
	return ns ? ns->width : 0U;
}

unsigned int gc573_no_signal_height(const struct gc573_no_signal *ns)
{
	return ns ? ns->height : 0U;
}

void gc573_no_signal_destroy(struct gc573_no_signal *ns)
{
	if (!ns)
		return;

	pthread_mutex_lock(&ns->lock);
	free(ns->pixels);
	ns->pixels = NULL;
	ns->width = 0;
	ns->height = 0;
	ns->bytes = 0;
	pthread_mutex_unlock(&ns->lock);
}
