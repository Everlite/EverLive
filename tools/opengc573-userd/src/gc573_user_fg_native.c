/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_fg_native.c — Built-in GPL-2.0 Motion-Compensated Frame Gen
 *
 * Prototype native interpolation engine that operates directly on EverLive's
 * YUYV frames with zero external binaries or proprietary DLLs.
 */
#include "gc573_user_fg_native.h"

#include <SDL2/SDL.h>
#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_opengl_glext.h>

#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FGN_DS		8U	/* 8x8 luma downsample factor for fast estimation */
#define FGN_BLK		4U	/* 4x4 blocks in downsampled space (= 32x32 px) */
#define FGN_SEARCH	6	/* +-6 ds px = +-48 full-res px */
#define FGN_HUD_STREAK	3U	/* frames a static block stays locked */

struct fgn_mv {
	int16_t dx;		/* full-res horizontal shift (pixels) */
	int16_t dy;		/* full-res vertical shift (pixels) */
	uint8_t hud_lock;	/* 1 if static HUD/UI block */
	uint8_t confidence;	/* 0..255 */
};

struct fgn_synth_job {
	const struct gc573_fg_native *ctx;
	uint8_t *out;
	uint32_t out_stride;
	float t;
	uint32_t y_start;
	uint32_t y_end;
};

struct gc573_fg_native {
	uint32_t width;
	uint32_t height;
	size_t frame_bytes;
	uint8_t *prev_yuyv;
	uint8_t *curr_yuyv;
	bool has_prev;
	bool has_curr;

	/* 4x4 downsampled luma buffers */
	uint32_t ds_w;
	uint32_t ds_h;
	uint8_t *prev_ds;
	uint8_t *curr_ds;

	/* Block motion vector grid */
	uint32_t grid_w;
	uint32_t grid_h;
	struct fgn_mv *mvs;
	struct fgn_mv *mvs_smooth;
	uint8_t *hud_streak;
	uint8_t *mv_rgba;
	uint64_t frame_seq;

	/* OpenGL GPU Optical Flow + interpolation state */
	float flow_scale;
	bool perf_mode;
	float gl_flow_scale;
	bool gl_tried;
	bool gl_ok;
	bool gl_fbo_ok;
	uint32_t gl_w;
	uint32_t gl_h;
	uint32_t flow_w;
	uint32_t flow_h;
	uint64_t gl_uploaded_seq;
	GLuint gl_prog;
	GLuint gl_prog_luma;
	GLuint gl_prog_down;
	GLuint gl_prog_flow;
	GLuint gl_prog_smooth;
	GLuint gl_fbo;
	GLuint gl_tex_prev;
	GLuint gl_tex_curr;
	GLuint gl_tex_mv;
	GLuint gl_tex_luma_p[3]; /* 0=1/4, 1=1/8, 2=1/16 */
	GLuint gl_tex_luma_c[3];
	GLuint gl_tex_flow[5];   /* 0=coarse, 1=mid, 2=fine, 3=smooth, 4=prev_smooth */
	GLint gl_u_prev;
	GLint gl_u_curr;
	GLint gl_u_mv;
	GLint gl_u_luma_c;
	GLint gl_u_t;
	GLint gl_u_size;
	GLint gl_u_flow_size;
	GLint gl_a_pos;
	GLint gl_luma_u_tex;
	GLint gl_luma_u_size;
	GLint gl_luma_a_pos;
	GLint gl_down_u_tex;
	GLint gl_down_u_texel;
	GLint gl_down_a_pos;
	GLint gl_flow_u_luma_p;
	GLint gl_flow_u_luma_c;
	GLint gl_flow_u_flow_in;
	GLint gl_flow_u_size;
	GLint gl_flow_u_lvl_size;
	GLint gl_flow_u_has_prior;
	GLint gl_flow_u_radius;
	GLint gl_flow_a_pos;
	GLint gl_smooth_u_flow;
	GLint gl_smooth_u_luma_p;
	GLint gl_smooth_u_luma_c;
	GLint gl_smooth_u_prev_flow;
	GLint gl_smooth_u_lvl_size;
	GLint gl_smooth_a_pos;
	PFNGLCREATESHADERPROC p_glCreateShader;
	PFNGLSHADERSOURCEPROC p_glShaderSource;
	PFNGLCOMPILESHADERPROC p_glCompileShader;
	PFNGLGETSHADERIVPROC p_glGetShaderiv;
	PFNGLCREATEPROGRAMPROC p_glCreateProgram;
	PFNGLATTACHSHADERPROC p_glAttachShader;
	PFNGLLINKPROGRAMPROC p_glLinkProgram;
	PFNGLGETPROGRAMIVPROC p_glGetProgramiv;
	PFNGLUSEPROGRAMPROC p_glUseProgram;
	PFNGLDELETESHADERPROC p_glDeleteShader;
	PFNGLDELETEPROGRAMPROC p_glDeleteProgram;
	PFNGLGETUNIFORMLOCATIONPROC p_glGetUniformLocation;
	PFNGLGETATTRIBLOCATIONPROC p_glGetAttribLocation;
	PFNGLUNIFORM1IPROC p_glUniform1i;
	PFNGLUNIFORM1FPROC p_glUniform1f;
	PFNGLUNIFORM2FPROC p_glUniform2f;
	PFNGLACTIVETEXTUREPROC p_glActiveTexture;
	PFNGLENABLEVERTEXATTRIBARRAYPROC p_glEnableVertexAttribArray;
	PFNGLDISABLEVERTEXATTRIBARRAYPROC p_glDisableVertexAttribArray;
	PFNGLVERTEXATTRIBPOINTERPROC p_glVertexAttribPointer;
	void (APIENTRY *p_glGenTextures)(GLsizei, GLuint *);
	void (APIENTRY *p_glDeleteTextures)(GLsizei, const GLuint *);
	void (APIENTRY *p_glBindTexture)(GLenum, GLuint);
	void (APIENTRY *p_glTexParameteri)(GLenum, GLenum, GLint);
	void (APIENTRY *p_glTexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
	void (APIENTRY *p_glTexSubImage2D)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *);
	void (APIENTRY *p_glViewport)(GLint, GLint, GLsizei, GLsizei);
	void (APIENTRY *p_glDrawArrays)(GLenum, GLint, GLsizei);
	void (APIENTRY *p_glPixelStorei)(GLenum, GLint);
	void (APIENTRY *p_glGetIntegerv)(GLenum, GLint *);
	void (APIENTRY *p_glDisable)(GLenum);
	void (APIENTRY *p_glEnable)(GLenum);
	GLboolean (APIENTRY *p_glIsEnabled)(GLenum);
	void (APIENTRY *p_glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
	void (APIENTRY *p_glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
	PFNGLBINDBUFFERPROC p_glBindBuffer;
	PFNGLGENFRAMEBUFFERSPROC p_glGenFramebuffers;
	PFNGLDELETEFRAMEBUFFERSPROC p_glDeleteFramebuffers;
	PFNGLBINDFRAMEBUFFERPROC p_glBindFramebuffer;
	PFNGLFRAMEBUFFERTEXTURE2DPROC p_glFramebufferTexture2D;
	PFNGLCHECKFRAMEBUFFERSTATUSPROC p_glCheckFramebufferStatus;
};

static inline int clamp_i(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static void fgn_free_buffers(struct gc573_fg_native *ctx)
{
	free(ctx->prev_yuyv);
	free(ctx->curr_yuyv);
	free(ctx->prev_ds);
	free(ctx->curr_ds);
	free(ctx->mvs);
	free(ctx->mvs_smooth);
	free(ctx->hud_streak);
	free(ctx->mv_rgba);
	ctx->prev_yuyv = NULL;
	ctx->curr_yuyv = NULL;
	ctx->prev_ds = NULL;
	ctx->curr_ds = NULL;
	ctx->mvs = NULL;
	ctx->mvs_smooth = NULL;
	ctx->hud_streak = NULL;
	ctx->mv_rgba = NULL;
	ctx->width = 0;
	ctx->height = 0;
	ctx->frame_bytes = 0;
	ctx->ds_w = 0;
	ctx->ds_h = 0;
	ctx->grid_w = 0;
	ctx->grid_h = 0;
	ctx->has_prev = false;
	ctx->has_curr = false;
}

static bool fgn_alloc_buffers(struct gc573_fg_native *ctx,
			      uint32_t width, uint32_t height)
{
	size_t n_blocks;

	fgn_free_buffers(ctx);
	if (width < 64 || height < 64 || width > 8192U || height > 4320U ||
	    (width & 1U))
		return false;

	ctx->width = width;
	ctx->height = height;
	ctx->frame_bytes = (size_t)width * (size_t)height * 2U;
	ctx->ds_w = width / FGN_DS;
	ctx->ds_h = height / FGN_DS;
	ctx->grid_w = (ctx->ds_w + FGN_BLK - 1U) / FGN_BLK;
	ctx->grid_h = (ctx->ds_h + FGN_BLK - 1U) / FGN_BLK;
	n_blocks = (size_t)ctx->grid_w * ctx->grid_h;

	ctx->prev_yuyv = malloc(ctx->frame_bytes);
	ctx->curr_yuyv = malloc(ctx->frame_bytes);
	ctx->prev_ds = malloc((size_t)ctx->ds_w * ctx->ds_h);
	ctx->curr_ds = malloc((size_t)ctx->ds_w * ctx->ds_h);
	ctx->mvs = calloc(n_blocks, sizeof(*ctx->mvs));
	ctx->mvs_smooth = calloc(n_blocks, sizeof(*ctx->mvs_smooth));
	ctx->hud_streak = calloc(n_blocks, 1);
	ctx->mv_rgba = calloc(n_blocks, 4);

	if (!ctx->prev_yuyv || !ctx->curr_yuyv || !ctx->prev_ds ||
	    !ctx->curr_ds || !ctx->mvs || !ctx->mvs_smooth ||
	    !ctx->hud_streak || !ctx->mv_rgba) {
		fgn_free_buffers(ctx);
		return false;
	}
	return true;
}

/* Downsample YUYV luma by 4x4 using 2x2 sparse tap average for speed */
static void fgn_downsample_luma(const uint8_t *yuyv, uint32_t w, uint32_t h,
				uint8_t *ds, uint32_t ds_w, uint32_t ds_h)
{
	uint32_t stride = w * 2U;

	for (uint32_t dy = 0; dy < ds_h; dy++) {
		uint32_t sy = dy * FGN_DS;
		const uint8_t *r0 = yuyv + (size_t)sy * stride;
		const uint8_t *r1 = yuyv + (size_t)(sy + 2U < h ? sy + 2U : sy) * stride;
		uint8_t *out = ds + (size_t)dy * ds_w;

		for (uint32_t dx = 0; dx < ds_w; dx++) {
			uint32_t sx2 = dx * FGN_DS * 2U;
			uint32_t sum = (uint32_t)r0[sx2] + r0[sx2 + 4U] +
				       (uint32_t)r1[sx2] + r1[sx2 + 4U];
			out[dx] = (uint8_t)(sum >> 2);
		}
	}
}

static uint32_t fgn_block_sad(const uint8_t *a, const uint8_t *b,
			      uint32_t ds_w, uint32_t ds_h,
			      int bx, int by, int dx, int dy)
{
	uint32_t sad = 0;

	for (int y = 0; y < (int)FGN_BLK; y++) {
		int py = clamp_i(by + y, 0, (int)ds_h - 1);
		int qy = clamp_i(by + y + dy, 0, (int)ds_h - 1);
		const uint8_t *ra = a + (size_t)py * ds_w;
		const uint8_t *rb = b + (size_t)qy * ds_w;

		for (int x = 0; x < (int)FGN_BLK; x++) {
			int px = clamp_i(bx + x, 0, (int)ds_w - 1);
			int qx = clamp_i(bx + x + dx, 0, (int)ds_w - 1);
			int diff = (int)ra[px] - (int)rb[qx];
			sad += (uint32_t)(diff < 0 ? -diff : diff);
		}
	}
	return sad;
}

static void fgn_estimate_motion(struct gc573_fg_native *ctx)
{
	static const int steps[] = { 4, 2, 1 };
	uint32_t gw = ctx->grid_w;
	uint32_t gh = ctx->grid_h;

	for (uint32_t gy = 0; gy < gh; gy++) {
		int by = (int)(gy * FGN_BLK);

		for (uint32_t gx = 0; gx < gw; gx++) {
			int bx = (int)(gx * FGN_BLK);
			size_t idx = (size_t)gy * gw + gx;
			uint32_t zero_sad = fgn_block_sad(
				ctx->prev_ds, ctx->curr_ds,
				ctx->ds_w, ctx->ds_h, bx, by, 0, 0);

			/* Static HUD Lock: blocks with near-zero change for several
			 * consecutive frames stay locked at (0,0) so UI doesn't smear. */
			if (zero_sad <= FGN_BLK * FGN_BLK * 1U) {
				if (ctx->hud_streak[idx] < 255)
					ctx->hud_streak[idx]++;
			} else {
				ctx->hud_streak[idx] = 0;
			}

			if (ctx->hud_streak[idx] >= FGN_HUD_STREAK) {
				ctx->mvs[idx].dx = 0;
				ctx->mvs[idx].dy = 0;
				ctx->mvs[idx].hud_lock = 1;
				ctx->mvs[idx].confidence = 255;
				continue;
			}

			/* Candidate predictors: (0,0) + left + top neighbor */
			int best_dx = 0;
			int best_dy = 0;
			uint32_t best_sad = zero_sad;

			if (gx > 0) {
				int pdx = ctx->mvs[idx - 1].dx / (int)(FGN_DS * 2U);
				int pdy = ctx->mvs[idx - 1].dy / (int)(FGN_DS * 2U);
				uint32_t s = fgn_block_sad(
					ctx->prev_ds, ctx->curr_ds,
					ctx->ds_w, ctx->ds_h, bx, by, pdx, pdy);
				if (s < best_sad) {
					best_sad = s;
					best_dx = pdx;
					best_dy = pdy;
				}
			}
			if (gy > 0) {
				int pdx = ctx->mvs[idx - gw].dx / (int)(FGN_DS * 2U);
				int pdy = ctx->mvs[idx - gw].dy / (int)(FGN_DS * 2U);
				uint32_t s = fgn_block_sad(
					ctx->prev_ds, ctx->curr_ds,
					ctx->ds_w, ctx->ds_h, bx, by, pdx, pdy);
				if (s < best_sad) {
					best_sad = s;
					best_dx = pdx;
					best_dy = pdy;
				}
			}

			/* Hierarchical 3-step diamond search */
			for (size_t si = 0; si < sizeof(steps) / sizeof(steps[0]); si++) {
				int step = steps[si];
				bool improved = true;
				int iters = 0;

				while (improved && iters++ < 4) {
					improved = false;
					int cx = best_dx;
					int cy = best_dy;
					static const int offs[4][2] = {
						{ 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 }
					};

					for (int k = 0; k < 4; k++) {
						int ndx = cx + offs[k][0] * step;
						int ndy = cy + offs[k][1] * step;
						uint32_t s;

						if (ndx < -FGN_SEARCH || ndx > FGN_SEARCH ||
						    ndy < -FGN_SEARCH || ndy > FGN_SEARCH)
							continue;
						s = fgn_block_sad(
							ctx->prev_ds, ctx->curr_ds,
							ctx->ds_w, ctx->ds_h,
							bx, by, ndx, ndy);
						/* Small penalty against spurious motion on flat regions */
						if (s + 16U < best_sad) {
							best_sad = s;
							best_dx = ndx;
							best_dy = ndy;
							improved = true;
						}
					}
				}
			}

			/* Reject weak/ambiguous motion that doesn't beat (0,0) by >= 20% */
			if ((best_dx != 0 || best_dy != 0) &&
			    best_sad * 5U >= zero_sad * 4U) {
				best_dx = 0;
				best_dy = 0;
				best_sad = zero_sad;
			}

			/* Parabolic sub-ds refinement -> half-pixel (0.5 px) precision,
			 * ONLY for genuine non-zero motion so static UI never shifts */
			{
				float sub_x = 0.0f;
				float sub_y = 0.0f;

				if (zero_sad > FGN_BLK * FGN_BLK * 2U) {
					if (best_dx > -FGN_SEARCH && best_dx < FGN_SEARCH) {
						uint32_t sl = fgn_block_sad(
							ctx->prev_ds, ctx->curr_ds,
							ctx->ds_w, ctx->ds_h, bx, by,
							best_dx - 1, best_dy);
						uint32_t sr = fgn_block_sad(
							ctx->prev_ds, ctx->curr_ds,
							ctx->ds_w, ctx->ds_h, bx, by,
							best_dx + 1, best_dy);
						int denom = 2 * ((int)sl + (int)sr - 2 * (int)best_sad);
						if (denom > 0) {
							sub_x = (float)((int)sl - (int)sr) / (float)denom;
							if (sub_x < -0.5f) sub_x = -0.5f;
							if (sub_x >  0.5f) sub_x =  0.5f;
						}
					}
					if (best_dy > -FGN_SEARCH && best_dy < FGN_SEARCH) {
						uint32_t su = fgn_block_sad(
							ctx->prev_ds, ctx->curr_ds,
							ctx->ds_w, ctx->ds_h, bx, by,
							best_dx, best_dy - 1);
						uint32_t sd = fgn_block_sad(
							ctx->prev_ds, ctx->curr_ds,
							ctx->ds_w, ctx->ds_h, bx, by,
							best_dx, best_dy + 1);
						int denom = 2 * ((int)su + (int)sd - 2 * (int)best_sad);
						if (denom > 0) {
							sub_y = (float)((int)su - (int)sd) / (float)denom;
							if (sub_y < -0.5f) sub_y = -0.5f;
							if (sub_y >  0.5f) sub_y =  0.5f;
						}
					}
				}
				ctx->mvs[idx].dx = (int16_t)lroundf(((float)best_dx + sub_x) * (float)(FGN_DS * 2U));
				ctx->mvs[idx].dy = (int16_t)lroundf(((float)best_dy + sub_y) * (float)(FGN_DS * 2U));
			}
			ctx->mvs[idx].hud_lock = 0;
			ctx->mvs[idx].confidence =
				best_sad < FGN_BLK * FGN_BLK * 12U ? 220 : 96;
		}
	}

	/* 3x3 spatial vector smoothing (never smear across HUD-locked blocks) */
	for (uint32_t gy = 0; gy < gh; gy++) {
		for (uint32_t gx = 0; gx < gw; gx++) {
			size_t idx = (size_t)gy * gw + gx;
			int sx = 0, sy = 0, cnt = 0;

			if (ctx->mvs[idx].hud_lock) {
				ctx->mvs_smooth[idx] = ctx->mvs[idx];
				continue;
			}
			for (int oy = -1; oy <= 1; oy++) {
				int ny = (int)gy + oy;
				if (ny < 0 || ny >= (int)gh)
					continue;
				for (int ox = -1; ox <= 1; ox++) {
					int nx = (int)gx + ox;
					size_t nidx;
					int w;

					if (nx < 0 || nx >= (int)gw)
						continue;
					nidx = (size_t)ny * gw + (uint32_t)nx;
					if (ctx->mvs[nidx].hud_lock)
						continue;
					w = (ox == 0 && oy == 0) ? 4 : 1;
					sx += ctx->mvs[nidx].dx * w;
					sy += ctx->mvs[nidx].dy * w;
					cnt += w;
				}
			}
			ctx->mvs_smooth[idx] = ctx->mvs[idx];
			if (cnt > 0) {
				ctx->mvs_smooth[idx].dx = (int16_t)(sx / cnt);
				ctx->mvs_smooth[idx].dy = (int16_t)(sy / cnt);
			}
		}
	}

	/* Pack smooth motion vectors + Static HUD Lock into 14 KB RGBA8 texture */
	for (uint32_t i = 0; i < gw * gh; i++) {
		int dx = clamp_i(128 + ctx->mvs_smooth[i].dx, 0, 255);
		int dy = clamp_i(128 + ctx->mvs_smooth[i].dy, 0, 255);
		ctx->mv_rgba[i * 4U + 0] = (uint8_t)dx;
		ctx->mv_rgba[i * 4U + 1] = (uint8_t)dy;
		ctx->mv_rgba[i * 4U + 2] = ctx->mvs_smooth[i].hud_lock ? 255 : 0;
		ctx->mv_rgba[i * 4U + 3] = ctx->mvs_smooth[i].confidence;
	}
}

static void fgn_synth_slice(const struct fgn_synth_job *job)
{
	const struct gc573_fg_native *ctx = job->ctx;
	uint32_t w = ctx->width;
	uint32_t h = ctx->height;
	uint32_t src_stride = w * 2U;
	int w_q8 = (int)(job->t * 256.0f + 0.5f);
	int inv_q8 = 256 - w_q8;
	uint32_t blk_px = FGN_DS * FGN_BLK;

	if (w_q8 < 0)
		w_q8 = 0;
	if (w_q8 > 256)
		w_q8 = 256;

	for (uint32_t y = job->y_start; y < job->y_end; y++) {
		uint32_t gy = y / blk_px;
		uint8_t *dst_row = job->out + (size_t)y * job->out_stride;
		const uint8_t *prev_same = ctx->prev_yuyv + (size_t)y * src_stride;
		const uint8_t *curr_same = ctx->curr_yuyv + (size_t)y * src_stride;

		if (gy >= ctx->grid_h)
			gy = ctx->grid_h - 1U;

		for (uint32_t gx = 0; gx < ctx->grid_w; gx++) {
			uint32_t x0 = gx * blk_px;
			uint32_t x1 = x0 + blk_px;
			struct fgn_mv mv = ctx->mvs_smooth[(size_t)gy * ctx->grid_w + gx];
			int off_px, off_py, off_qx, off_qy;
			int py, qy;
			const uint8_t *p_row;
			const uint8_t *q_row;

			if (x1 > w)
				x1 = w;
			if (x0 >= x1)
				continue;

			/* Static HUD Lock: memcpy crisp block from prev/curr */
			if (mv.hud_lock) {
				const uint8_t *src = (w_q8 < 128) ? prev_same : curr_same;
				memcpy(dst_row + (size_t)x0 * 2U,
				       src + (size_t)x0 * 2U,
				       (size_t)(x1 - x0) * 2U);
				continue;
			}

			/* Compute block displacement once per 32px block (mvs in 0.5 px units;
			 * keep sub-pixel precision: dx_px * w_q8 is in 1/512 px units) */
			{
				off_px = -((mv.dx * w_q8 + 256) >> 9);
				off_py = -((mv.dy * w_q8 + 256) >> 9);
				off_qx = (mv.dx * inv_q8 + 256) >> 9;
				off_qy = (mv.dy * inv_q8 + 256) >> 9;
			}

			py = clamp_i((int)y + off_py, 0, (int)h - 1);
			qy = clamp_i((int)y + off_qy, 0, (int)h - 1);
			p_row = ctx->prev_yuyv + (size_t)py * src_stride;
			q_row = ctx->curr_yuyv + (size_t)qy * src_stride;

			/* Fast path for interior blocks: zero per-pixel bounds clamping */
			if ((int)x0 + off_px >= 0 && (int)x1 + off_px <= (int)w &&
			    (int)x0 + off_qx >= 0 && (int)x1 + off_qx <= (int)w) {
				int px0 = ((int)x0 + off_px) & ~1;
				int qx0 = ((int)x0 + off_qx) & ~1;
				const uint8_t *p = p_row + (size_t)px0 * 2U;
				const uint8_t *q = q_row + (size_t)qx0 * 2U;
				uint8_t *d = dst_row + (size_t)x0 * 2U;
				size_t bytes = (size_t)(x1 - x0) * 2U;

				for (size_t i = 0; i < bytes; i++)
					d[i] = (uint8_t)((p[i] * inv_q8 + q[i] * w_q8) >> 8);
			} else {
				for (uint32_t x = x0; x < x1; x += 2U) {
					int px = clamp_i((int)x + off_px, 0, (int)w - 2) & ~1;
					int qx = clamp_i((int)x + off_qx, 0, (int)w - 2) & ~1;
					const uint8_t *p0 = p_row + (size_t)px * 2U;
					const uint8_t *q0 = q_row + (size_t)qx * 2U;
					uint8_t *d = dst_row + (size_t)x * 2U;

					d[0] = (uint8_t)((p0[0] * inv_q8 + q0[0] * w_q8) >> 8);
					d[1] = (uint8_t)((p0[1] * inv_q8 + q0[1] * w_q8) >> 8);
					d[2] = (uint8_t)((p0[2] * inv_q8 + q0[2] * w_q8) >> 8);
					d[3] = (uint8_t)((p0[3] * inv_q8 + q0[3] * w_q8) >> 8);
				}
			}
		}
	}
}

struct gc573_fg_native *gc573_fg_native_create(void)
{
	struct gc573_fg_native *ctx = calloc(1, sizeof(struct gc573_fg_native));

	if (ctx) {
		ctx->flow_scale = 1.0f;
		ctx->gl_flow_scale = 1.0f;
	}
	return ctx;
}

void gc573_fg_native_set_params(struct gc573_fg_native *ctx,
				float flow_scale,
				bool perf_mode)
{
	if (!ctx)
		return;
	if (flow_scale < 0.5f)
		flow_scale = 0.5f;
	if (flow_scale > 1.0f)
		flow_scale = 1.0f;
	ctx->flow_scale = flow_scale;
	ctx->perf_mode = perf_mode;
}

void gc573_fg_native_destroy(struct gc573_fg_native *ctx)
{
	if (!ctx)
		return;
	gc573_fg_native_gl_teardown(ctx);
	fgn_free_buffers(ctx);
	free(ctx);
}

void gc573_fg_native_reset(struct gc573_fg_native *ctx)
{
	if (!ctx)
		return;
	ctx->has_prev = false;
	ctx->has_curr = false;
	if (ctx->hud_streak && ctx->grid_w && ctx->grid_h)
		memset(ctx->hud_streak, 0, (size_t)ctx->grid_w * ctx->grid_h);
}

bool gc573_fg_native_push(struct gc573_fg_native *ctx,
			  const uint8_t *yuyv,
			  uint32_t width,
			  uint32_t height,
			  uint32_t stride_bytes)
{
	uint32_t row_bytes;

	if (!ctx || !yuyv || !width || !height)
		return false;

	if (ctx->width != width || ctx->height != height) {
		if (!fgn_alloc_buffers(ctx, width, height))
			return false;
	}

	/* Rotate curr -> prev */
	if (ctx->has_curr) {
		uint8_t *tmp_y = ctx->prev_yuyv;
		uint8_t *tmp_d = ctx->prev_ds;
		ctx->prev_yuyv = ctx->curr_yuyv;
		ctx->prev_ds = ctx->curr_ds;
		ctx->curr_yuyv = tmp_y;
		ctx->curr_ds = tmp_d;
		ctx->has_prev = true;
	}

	row_bytes = width * 2U;
	if (stride_bytes == 0 || stride_bytes == row_bytes) {
		memcpy(ctx->curr_yuyv, yuyv, ctx->frame_bytes);
	} else {
		for (uint32_t y = 0; y < height; y++)
			memcpy(ctx->curr_yuyv + (size_t)y * row_bytes,
			       yuyv + (size_t)y * stride_bytes, row_bytes);
	}
	if (!ctx->gl_ok) {
		fgn_downsample_luma(ctx->curr_yuyv, width, height,
				    ctx->curr_ds, ctx->ds_w, ctx->ds_h);
	}
	ctx->has_curr = true;
	ctx->frame_seq++;

	if (ctx->has_prev && !ctx->gl_ok)
		fgn_estimate_motion(ctx);

	return ctx->has_prev;
}

bool gc573_fg_native_ready(const struct gc573_fg_native *ctx)
{
	return ctx && ctx->has_prev && ctx->has_curr;
}

const uint8_t *gc573_fg_native_prev(const struct gc573_fg_native *ctx)
{
	return (ctx && ctx->has_prev) ? ctx->prev_yuyv : NULL;
}

bool gc573_fg_native_synthesize(struct gc573_fg_native *ctx,
				float t,
				uint8_t *out_yuyv,
				uint32_t out_stride_bytes)
{
	struct fgn_synth_job job;

	if (!gc573_fg_native_ready(ctx) || !out_yuyv)
		return false;

	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;

	job.ctx = ctx;
	job.out = out_yuyv;
	job.out_stride = out_stride_bytes ? out_stride_bytes : ctx->width * 2U;
	job.t = t;
	job.y_start = 0;
	job.y_end = ctx->height;
	fgn_synth_slice(&job);

	return true;
}

/* ------------------------------------------------------------------ */
/* OpenGL GPU Frame Interpolation (0-byte upload for synthetic frames) */
/* ------------------------------------------------------------------ */

static const char *fgn_vs_src =
	"#version 120\n"
	"attribute vec2 a_pos;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"    v_uv = vec2(a_pos.x * 0.5 + 0.5, 0.5 - a_pos.y * 0.5);\n"
	"    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

static const char *fgn_vs_fbo_src =
	"#version 120\n"
	"attribute vec2 a_pos;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"    v_uv = a_pos * 0.5 + 0.5;\n"
	"    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
	"}\n";

/* Pass 1: YUYV -> 1/4-res Luma via true 4x4 box filter (16 taps, zero aliasing) */
static const char *fgn_fs_luma_src =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_size;\n"
	"varying vec2 v_uv;\n"
	"float sample_luma(vec2 uv) {\n"
	"    vec2 cuv = clamp(uv, vec2(0.0), vec2(1.0));\n"
	"    float px = cuv.x * u_size.x;\n"
	"    float half_w = u_size.x * 0.5;\n"
	"    float mx = (floor(px * 0.5) + 0.5) / half_w;\n"
	"    vec4 p = texture2D(u_tex, vec2(mx, cuv.y));\n"
	"    return (mod(px, 2.0) < 1.0) ? p.r : p.b;\n"
	"}\n"
	"void main() {\n"
	"    vec2 d = 1.0 / u_size;\n"
	"    float sum = 0.0;\n"
	"    for (int j = 0; j < 4; j++) {\n"
	"        float oy = (float(j) - 1.5) * d.y;\n"
	"        for (int i = 0; i < 4; i++) {\n"
	"            float ox = (float(i) - 1.5) * d.x;\n"
	"            sum += sample_luma(v_uv + vec2(ox, oy));\n"
	"        }\n"
	"    }\n"
	"    float avg = sum * (1.0 / 16.0);\n"
	"    gl_FragColor = vec4(avg, avg, avg, 1.0);\n"
	"}\n";

/* Pass 2: 2x2 box downsample for luma pyramid */
static const char *fgn_fs_down_src =
	"#version 120\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_texel;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"    float a = texture2D(u_tex, v_uv + vec2(-0.5, -0.5) * u_texel).r;\n"
	"    float b = texture2D(u_tex, v_uv + vec2( 0.5, -0.5) * u_texel).r;\n"
	"    float c = texture2D(u_tex, v_uv + vec2(-0.5,  0.5) * u_texel).r;\n"
	"    float d = texture2D(u_tex, v_uv + vec2( 0.5,  0.5) * u_texel).r;\n"
	"    float v = 0.25 * (a + b + c + d);\n"
	"    gl_FragColor = vec4(v, v, v, 1.0);\n"
	"}\n";

/* Pass 3: Multi-scale dense optical flow search + temporal prior + parabolic sub-pixel */
static const char *fgn_fs_flow_src =
	"#version 120\n"
	"uniform sampler2D u_luma_p;\n"
	"uniform sampler2D u_luma_c;\n"
	"uniform sampler2D u_flow_in;\n"
	"uniform vec2 u_size;\n"      /* full-res size */
	"uniform vec2 u_lvl_size;\n"  /* this pyramid level size */
	"uniform float u_has_prior;\n"
	"uniform float u_radius;\n"
	"varying vec2 v_uv;\n"
	"\n"
	"float patch_sad(vec2 uv, vec2 shift_uv) {\n"
	"    vec2 dt = 2.0 / u_lvl_size;\n"
	"    float sad = 0.0;\n"
	"    for (int j = -1; j <= 1; j++) {\n"
	"        for (int i = -1; i <= 1; i++) {\n"
	"            vec2 o = vec2(float(i), float(j)) * dt;\n"
	"            float a = texture2D(u_luma_p, clamp(uv + o - shift_uv * 0.5, vec2(0.0), vec2(1.0))).r;\n"
	"            float b = texture2D(u_luma_c, clamp(uv + o + shift_uv * 0.5, vec2(0.0), vec2(1.0))).r;\n"
	"            sad += abs(a - b);\n"
	"        }\n"
	"    }\n"
	"    return sad * (1.0 / 9.0);\n"
	"}\n"
	"\n"
	"void main() {\n"
	"    vec2 prior_px = vec2(0.0);\n"
	"    if (u_has_prior > 0.5) {\n"
	"        vec2 enc = texture2D(u_flow_in, v_uv).rg;\n"
	"        prior_px = (enc - vec2(128.0 / 255.0)) * 160.0;\n"
	"    }\n"
	"    float scale_x = u_size.x / u_lvl_size.x;\n"
	"    float scale_y = u_size.y / u_lvl_size.y;\n"
	"    vec2 prior_lvl = prior_px / vec2(scale_x, scale_y);\n"
	"    float zero_sad = patch_sad(v_uv, vec2(0.0));\n"
	"    vec2 best_lvl = prior_lvl;\n"
	"    float best_sad = patch_sad(v_uv, best_lvl / u_lvl_size);\n"
	"    if (zero_sad <= best_sad) {\n"
	"        best_lvl = vec2(0.0);\n"
	"        best_sad = zero_sad;\n"
	"    }\n"
	"    vec2 center = best_lvl;\n"
	"    int rad = int(u_radius + 0.5);\n"
	"    for (int dy = -4; dy <= 4; dy++) {\n"
	"        if (dy < -rad || dy > rad) continue;\n"
	"        for (int dx = -4; dx <= 4; dx++) {\n"
	"            if (dx < -rad || dx > rad) continue;\n"
	"            vec2 cand = center + vec2(float(dx), float(dy));\n"
	"            float s = patch_sad(v_uv, cand / u_lvl_size);\n"
	"            if (s + 0.0005 < best_sad) {\n"
	"                best_sad = s;\n"
	"                best_lvl = cand;\n"
	"            }\n"
	"        }\n"
	"    }\n"
	"    vec2 sub = vec2(0.0);\n"
	"    if (zero_sad > 0.002) {\n"
	"        float sl = patch_sad(v_uv, (best_lvl + vec2(-1.0, 0.0)) / u_lvl_size);\n"
	"        float sr = patch_sad(v_uv, (best_lvl + vec2( 1.0, 0.0)) / u_lvl_size);\n"
	"        float dx_den = 2.0 * (sl + sr - 2.0 * best_sad);\n"
	"        if (dx_den > 0.0002) sub.x = clamp((sl - sr) / dx_den, -0.5, 0.5);\n"
	"        float su = patch_sad(v_uv, (best_lvl + vec2(0.0, -1.0)) / u_lvl_size);\n"
	"        float sd = patch_sad(v_uv, (best_lvl + vec2(0.0,  1.0)) / u_lvl_size);\n"
	"        float dy_den = 2.0 * (su + sd - 2.0 * best_sad);\n"
	"        if (dy_den > 0.0002) sub.y = clamp((su - sd) / dy_den, -0.5, 0.5);\n"
	"        best_sad = min(best_sad, patch_sad(v_uv, (best_lvl + sub) / u_lvl_size));\n"
	"    }\n"
	"    vec2 final_px = (best_lvl + sub) * vec2(scale_x, scale_y);\n"
	"    if (zero_sad < 0.002 || best_sad >= zero_sad) final_px = vec2(0.0);\n"
	"    vec2 enc_out = clamp(final_px / 160.0 + vec2(128.0 / 255.0), 0.0, 1.0);\n"
	"    float conf = clamp(1.0 - best_sad * 2.0, 0.0, 1.0);\n"
	"    gl_FragColor = vec4(enc_out, 0.0, conf);\n"
	"}\n";

/* Pass 4: Edge-preserving bilateral flow smoothing + temporal static HUD hysteresis */
static const char *fgn_fs_smooth_src =
	"#version 120\n"
	"uniform sampler2D u_flow;\n"
	"uniform sampler2D u_prev_flow;\n"
	"uniform sampler2D u_luma_p;\n"
	"uniform sampler2D u_luma_c;\n"
	"uniform vec2 u_lvl_size;\n"
	"varying vec2 v_uv;\n"
	"void main() {\n"
	"    vec2 dt = 1.0 / u_lvl_size;\n"
	"    vec4 center = texture2D(u_flow, v_uv);\n"
	"    float prev_streak = texture2D(u_prev_flow, v_uv).b;\n"
	"    float lc = texture2D(u_luma_c, v_uv).r;\n"
	"    float lp = texture2D(u_luma_p, v_uv).r;\n"
	"    float diff_mean = (abs(lp - lc)\n"
	"        + abs(texture2D(u_luma_p, v_uv + vec2( 2.0 * dt.x, 0.0)).r - texture2D(u_luma_c, v_uv + vec2( 2.0 * dt.x, 0.0)).r)\n"
	"        + abs(texture2D(u_luma_p, v_uv + vec2(-2.0 * dt.x, 0.0)).r - texture2D(u_luma_c, v_uv + vec2(-2.0 * dt.x, 0.0)).r)\n"
	"        + abs(texture2D(u_luma_p, v_uv + vec2(0.0,  2.0 * dt.y)).r - texture2D(u_luma_c, v_uv + vec2(0.0,  2.0 * dt.y)).r)\n"
	"        + abs(texture2D(u_luma_p, v_uv + vec2(0.0, -2.0 * dt.y)).r - texture2D(u_luma_c, v_uv + vec2(0.0, -2.0 * dt.y)).r)) * 0.20;\n"
	"    float streak = (diff_mean < 0.015) ? min(prev_streak + 0.25, 1.0) : max(prev_streak - 0.50, 0.0);\n"
	"    if (streak >= 0.90) {\n"
	"        gl_FragColor = vec4(128.0 / 255.0, 128.0 / 255.0, streak, 1.0);\n"
	"        return;\n"
	"    }\n"
	"    vec2 sum_f = vec2(0.0);\n"
	"    float sum_w = 0.0;\n"
	"    for (int j = -2; j <= 2; j++) {\n"
	"        for (int i = -2; i <= 2; i++) {\n"
	"            vec2 uv_n = clamp(v_uv + vec2(float(i), float(j)) * dt, vec2(0.0), vec2(1.0));\n"
	"            vec4 fn = texture2D(u_flow, uv_n);\n"
	"            float ln = texture2D(u_luma_c, uv_n).r;\n"
	"            float w = exp(-abs(ln - lc) * 16.0) * (0.2 + fn.a);\n"
	"            sum_f += fn.rg * w;\n"
	"            sum_w += w;\n"
	"        }\n"
	"    }\n"
	"    vec2 out_rg = (sum_w > 0.001) ? (sum_f / sum_w) : center.rg;\n"
	"    gl_FragColor = vec4(out_rg, streak, center.a);\n"
	"}\n";

static const char *fgn_fs_src =
	"#version 120\n"
	"uniform sampler2D u_prev;\n"
	"uniform sampler2D u_curr;\n"
	"uniform sampler2D u_mv;\n"
	"uniform sampler2D u_luma_c;\n"
	"uniform float u_t;\n"
	"uniform vec2 u_size;\n"
	"uniform vec2 u_flow_size;\n"
	"varying vec2 v_uv;\n"
	"\n"
	"vec3 yuyv_texel(sampler2D tex, float xi, float v, out float luma_out) {\n"
	"    float half_w = u_size.x * 0.5;\n"
	"    float cxi = clamp(xi, 0.0, u_size.x - 1.0);\n"
	"    float mx = (floor(cxi * 0.5) + 0.5) / half_w;\n"
	"    vec4 p = texture2D(tex, vec2(mx, v));\n"
	"    float y_raw = (mod(cxi, 2.0) < 1.0) ? p.r : p.b;\n"
	"    luma_out = y_raw;\n"
	"    float Y = (y_raw * 255.0 - 16.0) / 219.0;\n"
	"    float U = (p.g * 255.0 - 128.0) / 224.0;\n"
	"    float V = (p.a * 255.0 - 128.0) / 224.0;\n"
	"    return clamp(vec3(\n"
	"        Y + 1.5748 * V,\n"
	"        Y - 0.187324 * U - 0.468124 * V,\n"
	"        Y + 1.8556 * U\n"
	"    ), 0.0, 1.0);\n"
	"}\n"
	"\n"
	"vec3 sample_yuyv(sampler2D tex, vec2 uv) {\n"
	"    vec2 cuv = clamp(uv, vec2(0.0), vec2(1.0));\n"
	"    float px = cuv.x * u_size.x - 0.5;\n"
	"    float py = cuv.y * u_size.y - 0.5;\n"
	"    float x0 = floor(px);\n"
	"    float y0 = floor(py);\n"
	"    float fx = px - x0;\n"
	"    float fy = py - y0;\n"
	"    float v0 = clamp((y0 + 0.5) / u_size.y, 0.0, 1.0);\n"
	"    float v1 = clamp((y0 + 1.5) / u_size.y, 0.0, 1.0);\n"
	"    float l0, l1, l2, l3;\n"
	"    vec3 c00 = yuyv_texel(tex, x0,       v0, l0);\n"
	"    vec3 c10 = yuyv_texel(tex, x0 + 1.0, v0, l1);\n"
	"    vec3 c01 = yuyv_texel(tex, x0,       v1, l2);\n"
	"    vec3 c11 = yuyv_texel(tex, x0 + 1.0, v1, l3);\n"
	"    return mix(mix(c00, c10, fx), mix(c01, c11, fx), fy);\n"
	"}\n"
	"\n"
	"vec4 sample_flow_bilateral(vec2 uv, float luma_ref) {\n"
	"    vec2 fpos = uv * u_flow_size - 0.5;\n"
	"    vec2 f0 = floor(fpos);\n"
	"    vec2 ff = fpos - f0;\n"
	"    vec4 sum = vec4(0.0);\n"
	"    float wsum = 0.0;\n"
	"    for (int j = 0; j <= 1; j++) {\n"
	"        for (int i = 0; i <= 1; i++) {\n"
	"            vec2 suv = clamp((f0 + vec2(float(i), float(j)) + 0.5) / u_flow_size, vec2(0.0), vec2(1.0));\n"
	"            vec4 mv_s = texture2D(u_mv, suv);\n"
	"            float l_s = texture2D(u_luma_c, suv).r;\n"
	"            float wb = ((i == 0) ? (1.0 - ff.x) : ff.x) * ((j == 0) ? (1.0 - ff.y) : ff.y);\n"
	"            float wl = exp(-abs(l_s - luma_ref) * 16.0);\n"
	"            float w = wb * wl + 0.0001;\n"
	"            sum += mv_s * w;\n"
	"            wsum += w;\n"
	"        }\n"
	"    }\n"
	"    return sum / wsum;\n"
	"}\n"
	"\n"
	"void main() {\n"
	"    if (u_t <= 0.001) {\n"
	"        gl_FragColor = vec4(sample_yuyv(u_prev, v_uv), 1.0);\n"
	"        return;\n"
	"    }\n"
	"    if (u_t >= 0.999) {\n"
	"        gl_FragColor = vec4(sample_yuyv(u_curr, v_uv), 1.0);\n"
	"        return;\n"
	"    }\n"
	"    float l_p, l_q;\n"
	"    vec3 same_p = yuyv_texel(u_prev, floor(v_uv.x * u_size.x), v_uv.y, l_p);\n"
	"    vec3 same_q = yuyv_texel(u_curr, floor(v_uv.x * u_size.x), v_uv.y, l_q);\n"
	"    float luma_ref = l_q;\n"
	"    vec4 mvs = sample_flow_bilateral(v_uv, luma_ref);\n"
	"    float same_diff = dot(abs(same_p - same_q), vec3(0.3333));\n"
	"    vec3 blend_fb = mix(same_p, same_q, u_t);\n"
	"    vec2 mv = (mvs.rg - vec2(128.0 / 255.0)) * 160.0 / u_size;\n"
	"    vec2 uv_p = v_uv - mv * u_t;\n"
	"    vec2 uv_q = v_uv + mv * (1.0 - u_t);\n"
	"    vec3 cp = sample_yuyv(u_prev, uv_p);\n"
	"    vec3 cq = sample_yuyv(u_curr, uv_q);\n"
	"    float diff = dot(abs(cp - cq), vec3(0.3333));\n"
	"    float occ = smoothstep(0.10, 0.28, diff);\n"
	"    vec3 interp_c = mix(cp, cq, u_t);\n"
	"    vec3 motion_c = mix(interp_c, blend_fb, occ * 0.75);\n"
	"    float conf = smoothstep(0.10, 0.50, mvs.a);\n"
	"    vec3 out_c = mix(blend_fb, motion_c, conf);\n"
	"    float hud_w = smoothstep(0.70, 0.90, mvs.b) * (1.0 - smoothstep(0.012, 0.035, same_diff));\n"
	"    gl_FragColor = vec4(mix(out_c, blend_fb, hud_w), 1.0);\n"
	"}\n";

static GLuint fgn_compile_shader(struct gc573_fg_native *ctx,
				 GLenum type, const char *src)
{
	GLuint sh = ctx->p_glCreateShader(type);
	GLint ok = 0;

	if (!sh)
		return 0;
	ctx->p_glShaderSource(sh, 1, &src, NULL);
	ctx->p_glCompileShader(sh);
	ctx->p_glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[512] = { 0 };
		if (ctx->p_glGetShaderInfoLog)
			ctx->p_glGetShaderInfoLog(sh, (GLsizei)sizeof(log), NULL, log);
		fprintf(stderr, "[FG-NATIVE] Shader compile failed: %s\n", log);
		ctx->p_glDeleteShader(sh);
		return 0;
	}
	return sh;
}

static GLuint fgn_link_program(struct gc573_fg_native *ctx,
			       const char *vs_src, const char *fs_src)
{
	GLuint vs = fgn_compile_shader(ctx, GL_VERTEX_SHADER, vs_src);
	GLuint fs = fgn_compile_shader(ctx, GL_FRAGMENT_SHADER, fs_src);
	GLuint prog;
	GLint linked = 0;

	if (!vs || !fs) {
		if (vs) ctx->p_glDeleteShader(vs);
		if (fs) ctx->p_glDeleteShader(fs);
		return 0;
	}
	prog = ctx->p_glCreateProgram();
	ctx->p_glAttachShader(prog, vs);
	ctx->p_glAttachShader(prog, fs);
	ctx->p_glLinkProgram(prog);
	ctx->p_glDeleteShader(vs);
	ctx->p_glDeleteShader(fs);
	ctx->p_glGetProgramiv(prog, GL_LINK_STATUS, &linked);
	if (!linked) {
		char log[512] = { 0 };
		if (ctx->p_glGetProgramInfoLog)
			ctx->p_glGetProgramInfoLog(prog, (GLsizei)sizeof(log), NULL, log);
		fprintf(stderr, "[FG-NATIVE] Program link failed: %s\n", log);
		ctx->p_glDeleteProgram(prog);
		return 0;
	}
	return prog;
}

static bool fgn_gl_init(struct gc573_fg_native *ctx)
{
	if (ctx->gl_tried)
		return ctx->gl_ok;
	ctx->gl_tried = true;

	if (!SDL_GL_GetCurrentContext())
		return false;

	ctx->p_glCreateShader = (PFNGLCREATESHADERPROC)SDL_GL_GetProcAddress("glCreateShader");
	ctx->p_glShaderSource = (PFNGLSHADERSOURCEPROC)SDL_GL_GetProcAddress("glShaderSource");
	ctx->p_glCompileShader = (PFNGLCOMPILESHADERPROC)SDL_GL_GetProcAddress("glCompileShader");
	ctx->p_glGetShaderiv = (PFNGLGETSHADERIVPROC)SDL_GL_GetProcAddress("glGetShaderiv");
	ctx->p_glCreateProgram = (PFNGLCREATEPROGRAMPROC)SDL_GL_GetProcAddress("glCreateProgram");
	ctx->p_glAttachShader = (PFNGLATTACHSHADERPROC)SDL_GL_GetProcAddress("glAttachShader");
	ctx->p_glLinkProgram = (PFNGLLINKPROGRAMPROC)SDL_GL_GetProcAddress("glLinkProgram");
	ctx->p_glGetProgramiv = (PFNGLGETPROGRAMIVPROC)SDL_GL_GetProcAddress("glGetProgramiv");
	ctx->p_glUseProgram = (PFNGLUSEPROGRAMPROC)SDL_GL_GetProcAddress("glUseProgram");
	ctx->p_glDeleteShader = (PFNGLDELETESHADERPROC)SDL_GL_GetProcAddress("glDeleteShader");
	ctx->p_glDeleteProgram = (PFNGLDELETEPROGRAMPROC)SDL_GL_GetProcAddress("glDeleteProgram");
	ctx->p_glGetUniformLocation = (PFNGLGETUNIFORMLOCATIONPROC)SDL_GL_GetProcAddress("glGetUniformLocation");
	ctx->p_glGetAttribLocation = (PFNGLGETATTRIBLOCATIONPROC)SDL_GL_GetProcAddress("glGetAttribLocation");
	ctx->p_glUniform1i = (PFNGLUNIFORM1IPROC)SDL_GL_GetProcAddress("glUniform1i");
	ctx->p_glUniform1f = (PFNGLUNIFORM1FPROC)SDL_GL_GetProcAddress("glUniform1f");
	ctx->p_glUniform2f = (PFNGLUNIFORM2FPROC)SDL_GL_GetProcAddress("glUniform2f");
	ctx->p_glActiveTexture = (PFNGLACTIVETEXTUREPROC)SDL_GL_GetProcAddress("glActiveTexture");
	ctx->p_glEnableVertexAttribArray = (PFNGLENABLEVERTEXATTRIBARRAYPROC)SDL_GL_GetProcAddress("glEnableVertexAttribArray");
	ctx->p_glDisableVertexAttribArray = (PFNGLDISABLEVERTEXATTRIBARRAYPROC)SDL_GL_GetProcAddress("glDisableVertexAttribArray");
	ctx->p_glVertexAttribPointer = (PFNGLVERTEXATTRIBPOINTERPROC)SDL_GL_GetProcAddress("glVertexAttribPointer");
	ctx->p_glGenTextures = (void (APIENTRY *)(GLsizei, GLuint *))SDL_GL_GetProcAddress("glGenTextures");
	ctx->p_glDeleteTextures = (void (APIENTRY *)(GLsizei, const GLuint *))SDL_GL_GetProcAddress("glDeleteTextures");
	ctx->p_glBindTexture = (void (APIENTRY *)(GLenum, GLuint))SDL_GL_GetProcAddress("glBindTexture");
	ctx->p_glTexParameteri = (void (APIENTRY *)(GLenum, GLenum, GLint))SDL_GL_GetProcAddress("glTexParameteri");
	ctx->p_glTexImage2D = (void (APIENTRY *)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *))SDL_GL_GetProcAddress("glTexImage2D");
	ctx->p_glTexSubImage2D = (void (APIENTRY *)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void *))SDL_GL_GetProcAddress("glTexSubImage2D");
	ctx->p_glViewport = (void (APIENTRY *)(GLint, GLint, GLsizei, GLsizei))SDL_GL_GetProcAddress("glViewport");
	ctx->p_glDrawArrays = (void (APIENTRY *)(GLenum, GLint, GLsizei))SDL_GL_GetProcAddress("glDrawArrays");
	ctx->p_glPixelStorei = (void (APIENTRY *)(GLenum, GLint))SDL_GL_GetProcAddress("glPixelStorei");
	ctx->p_glGetIntegerv = (void (APIENTRY *)(GLenum, GLint *))SDL_GL_GetProcAddress("glGetIntegerv");
	ctx->p_glDisable = (void (APIENTRY *)(GLenum))SDL_GL_GetProcAddress("glDisable");
	ctx->p_glEnable = (void (APIENTRY *)(GLenum))SDL_GL_GetProcAddress("glEnable");
	ctx->p_glIsEnabled = (GLboolean (APIENTRY *)(GLenum))SDL_GL_GetProcAddress("glIsEnabled");
	ctx->p_glGetShaderInfoLog = (void (APIENTRY *)(GLuint, GLsizei, GLsizei *, GLchar *))SDL_GL_GetProcAddress("glGetShaderInfoLog");
	ctx->p_glGetProgramInfoLog = (void (APIENTRY *)(GLuint, GLsizei, GLsizei *, GLchar *))SDL_GL_GetProcAddress("glGetProgramInfoLog");
	ctx->p_glBindBuffer = (PFNGLBINDBUFFERPROC)SDL_GL_GetProcAddress("glBindBuffer");
	ctx->p_glGenFramebuffers = (PFNGLGENFRAMEBUFFERSPROC)SDL_GL_GetProcAddress("glGenFramebuffers");
	ctx->p_glDeleteFramebuffers = (PFNGLDELETEFRAMEBUFFERSPROC)SDL_GL_GetProcAddress("glDeleteFramebuffers");
	ctx->p_glBindFramebuffer = (PFNGLBINDFRAMEBUFFERPROC)SDL_GL_GetProcAddress("glBindFramebuffer");
	ctx->p_glFramebufferTexture2D = (PFNGLFRAMEBUFFERTEXTURE2DPROC)SDL_GL_GetProcAddress("glFramebufferTexture2D");
	ctx->p_glCheckFramebufferStatus = (PFNGLCHECKFRAMEBUFFERSTATUSPROC)SDL_GL_GetProcAddress("glCheckFramebufferStatus");

	if (!ctx->p_glCreateShader || !ctx->p_glShaderSource ||
	    !ctx->p_glCompileShader || !ctx->p_glGetShaderiv ||
	    !ctx->p_glCreateProgram || !ctx->p_glAttachShader ||
	    !ctx->p_glLinkProgram || !ctx->p_glGetProgramiv ||
	    !ctx->p_glUseProgram || !ctx->p_glDeleteShader ||
	    !ctx->p_glDeleteProgram || !ctx->p_glGetUniformLocation ||
	    !ctx->p_glGetAttribLocation || !ctx->p_glUniform1i ||
	    !ctx->p_glUniform1f || !ctx->p_glUniform2f ||
	    !ctx->p_glActiveTexture || !ctx->p_glEnableVertexAttribArray ||
	    !ctx->p_glDisableVertexAttribArray || !ctx->p_glVertexAttribPointer ||
	    !ctx->p_glGenTextures || !ctx->p_glDeleteTextures ||
	    !ctx->p_glBindTexture || !ctx->p_glTexParameteri ||
	    !ctx->p_glTexImage2D || !ctx->p_glTexSubImage2D ||
	    !ctx->p_glViewport || !ctx->p_glDrawArrays ||
	    !ctx->p_glPixelStorei || !ctx->p_glGetIntegerv ||
	    !ctx->p_glDisable || !ctx->p_glEnable || !ctx->p_glIsEnabled ||
	    !ctx->p_glGenFramebuffers || !ctx->p_glDeleteFramebuffers ||
	    !ctx->p_glBindFramebuffer || !ctx->p_glFramebufferTexture2D ||
	    !ctx->p_glCheckFramebufferStatus)
		return false;

	ctx->gl_prog        = fgn_link_program(ctx, fgn_vs_src, fgn_fs_src);
	ctx->gl_prog_luma   = fgn_link_program(ctx, fgn_vs_fbo_src, fgn_fs_luma_src);
	ctx->gl_prog_down   = fgn_link_program(ctx, fgn_vs_fbo_src, fgn_fs_down_src);
	ctx->gl_prog_flow   = fgn_link_program(ctx, fgn_vs_fbo_src, fgn_fs_flow_src);
	ctx->gl_prog_smooth = fgn_link_program(ctx, fgn_vs_fbo_src, fgn_fs_smooth_src);
	if (!ctx->gl_prog || !ctx->gl_prog_luma || !ctx->gl_prog_down ||
	    !ctx->gl_prog_flow || !ctx->gl_prog_smooth) {
		printf("[FG-NATIVE] GL init FAILED: shader/program link error\n");
		gc573_fg_native_gl_teardown(ctx);
		ctx->gl_tried = true;
		return false;
	}

	ctx->gl_u_prev      = ctx->p_glGetUniformLocation(ctx->gl_prog, "u_prev");
	ctx->gl_u_curr      = ctx->p_glGetUniformLocation(ctx->gl_prog, "u_curr");
	ctx->gl_u_mv        = ctx->p_glGetUniformLocation(ctx->gl_prog, "u_mv");
	ctx->gl_u_luma_c    = ctx->p_glGetUniformLocation(ctx->gl_prog, "u_luma_c");
	ctx->gl_u_t         = ctx->p_glGetUniformLocation(ctx->gl_prog, "u_t");
	ctx->gl_u_size      = ctx->p_glGetUniformLocation(ctx->gl_prog, "u_size");
	ctx->gl_u_flow_size = ctx->p_glGetUniformLocation(ctx->gl_prog, "u_flow_size");
	ctx->gl_a_pos       = ctx->p_glGetAttribLocation(ctx->gl_prog, "a_pos");

	ctx->gl_luma_u_tex  = ctx->p_glGetUniformLocation(ctx->gl_prog_luma, "u_tex");
	ctx->gl_luma_u_size = ctx->p_glGetUniformLocation(ctx->gl_prog_luma, "u_size");
	ctx->gl_luma_a_pos  = ctx->p_glGetAttribLocation(ctx->gl_prog_luma, "a_pos");

	ctx->gl_down_u_tex   = ctx->p_glGetUniformLocation(ctx->gl_prog_down, "u_tex");
	ctx->gl_down_u_texel = ctx->p_glGetUniformLocation(ctx->gl_prog_down, "u_texel");
	ctx->gl_down_a_pos   = ctx->p_glGetAttribLocation(ctx->gl_prog_down, "a_pos");

	ctx->gl_flow_u_luma_p    = ctx->p_glGetUniformLocation(ctx->gl_prog_flow, "u_luma_p");
	ctx->gl_flow_u_luma_c    = ctx->p_glGetUniformLocation(ctx->gl_prog_flow, "u_luma_c");
	ctx->gl_flow_u_flow_in   = ctx->p_glGetUniformLocation(ctx->gl_prog_flow, "u_flow_in");
	ctx->gl_flow_u_size      = ctx->p_glGetUniformLocation(ctx->gl_prog_flow, "u_size");
	ctx->gl_flow_u_lvl_size  = ctx->p_glGetUniformLocation(ctx->gl_prog_flow, "u_lvl_size");
	ctx->gl_flow_u_has_prior = ctx->p_glGetUniformLocation(ctx->gl_prog_flow, "u_has_prior");
	ctx->gl_flow_u_radius    = ctx->p_glGetUniformLocation(ctx->gl_prog_flow, "u_radius");
	ctx->gl_flow_a_pos       = ctx->p_glGetAttribLocation(ctx->gl_prog_flow, "a_pos");

	ctx->gl_smooth_u_flow      = ctx->p_glGetUniformLocation(ctx->gl_prog_smooth, "u_flow");
	ctx->gl_smooth_u_luma_p    = ctx->p_glGetUniformLocation(ctx->gl_prog_smooth, "u_luma_p");
	ctx->gl_smooth_u_luma_c    = ctx->p_glGetUniformLocation(ctx->gl_prog_smooth, "u_luma_c");
	ctx->gl_smooth_u_prev_flow = ctx->p_glGetUniformLocation(ctx->gl_prog_smooth, "u_prev_flow");
	ctx->gl_smooth_u_lvl_size  = ctx->p_glGetUniformLocation(ctx->gl_prog_smooth, "u_lvl_size");
	ctx->gl_smooth_a_pos       = ctx->p_glGetAttribLocation(ctx->gl_prog_smooth, "a_pos");

	ctx->p_glGenFramebuffers(1, &ctx->gl_fbo);
	ctx->p_glGenTextures(1, &ctx->gl_tex_prev);
	ctx->p_glGenTextures(1, &ctx->gl_tex_curr);
	ctx->p_glGenTextures(1, &ctx->gl_tex_mv);
	ctx->p_glGenTextures(3, ctx->gl_tex_luma_p);
	ctx->p_glGenTextures(3, ctx->gl_tex_luma_c);
	ctx->p_glGenTextures(5, ctx->gl_tex_flow);

	ctx->gl_ok = true;
	printf("[FG-NATIVE] GL init OK (GPU Multi-Scale Optical Flow)\n");
	return true;
}

static void fgn_alloc_tex2d(struct gc573_fg_native *ctx, GLuint tex,
			    uint32_t w, uint32_t h, GLint filter)
{
	ctx->p_glBindTexture(GL_TEXTURE_2D, tex);
	ctx->p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
	ctx->p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
	ctx->p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	ctx->p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	ctx->p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8,
			    (GLsizei)w, (GLsizei)h, 0,
			    GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

static void fgn_gl_alloc_textures(struct gc573_fg_native *ctx)
{
	float scale = (ctx->flow_scale >= 0.5f && ctx->flow_scale <= 1.0f)
		      ? ctx->flow_scale : 1.0f;
	uint32_t w4 = (uint32_t)((float)ctx->width * 0.25f * scale + 0.5f);
	uint32_t h4 = (uint32_t)((float)ctx->height * 0.25f * scale + 0.5f);
	if (w4 < 32U)
		w4 = 32U;
	if (h4 < 32U)
		h4 = 32U;
	uint32_t w8 = w4 / 2U > 16U ? w4 / 2U : 16U;
	uint32_t h8 = h4 / 2U > 16U ? h4 / 2U : 16U;
	uint32_t w16 = w8 / 2U > 8U ? w8 / 2U : 8U;
	uint32_t h16 = h8 / 2U > 8U ? h8 / 2U : 8U;

	fgn_alloc_tex2d(ctx, ctx->gl_tex_prev, ctx->width / 2U, ctx->height, GL_NEAREST);
	fgn_alloc_tex2d(ctx, ctx->gl_tex_curr, ctx->width / 2U, ctx->height, GL_NEAREST);
	fgn_alloc_tex2d(ctx, ctx->gl_tex_mv, ctx->grid_w, ctx->grid_h, GL_LINEAR);

	fgn_alloc_tex2d(ctx, ctx->gl_tex_luma_p[0], w4,  h4,  GL_LINEAR);
	fgn_alloc_tex2d(ctx, ctx->gl_tex_luma_c[0], w4,  h4,  GL_LINEAR);
	fgn_alloc_tex2d(ctx, ctx->gl_tex_luma_p[1], w8,  h8,  GL_LINEAR);
	fgn_alloc_tex2d(ctx, ctx->gl_tex_luma_c[1], w8,  h8,  GL_LINEAR);
	fgn_alloc_tex2d(ctx, ctx->gl_tex_luma_p[2], w16, h16, GL_LINEAR);
	fgn_alloc_tex2d(ctx, ctx->gl_tex_luma_c[2], w16, h16, GL_LINEAR);

	fgn_alloc_tex2d(ctx, ctx->gl_tex_flow[0], w16, h16, GL_LINEAR);
	fgn_alloc_tex2d(ctx, ctx->gl_tex_flow[1], w8,  h8,  GL_LINEAR);
	fgn_alloc_tex2d(ctx, ctx->gl_tex_flow[2], w4,  h4,  GL_LINEAR);
	fgn_alloc_tex2d(ctx, ctx->gl_tex_flow[3], w4,  h4,  GL_NEAREST);
	fgn_alloc_tex2d(ctx, ctx->gl_tex_flow[4], w4,  h4,  GL_NEAREST);

	/* Neutralize flow history texture (128, 128, 0, 0) */
	{
		size_t sz = (size_t)w4 * h4 * 4U;
		uint8_t *neutral = malloc(sz);
		if (neutral) {
			GLint old_rl = 0, old_sp = 0, old_sr = 0, old_al = 4;

			ctx->p_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &old_rl);
			ctx->p_glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &old_sp);
			ctx->p_glGetIntegerv(GL_UNPACK_SKIP_ROWS, &old_sr);
			ctx->p_glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_al);
			ctx->p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
			ctx->p_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
			ctx->p_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
			ctx->p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

			for (size_t i = 0; i < sz; i += 4U) {
				neutral[i + 0] = 128;
				neutral[i + 1] = 128;
				neutral[i + 2] = 0;
				neutral[i + 3] = 0;
			}
			ctx->p_glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_flow[4]);
			ctx->p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
					       (GLsizei)w4, (GLsizei)h4,
					       GL_RGBA, GL_UNSIGNED_BYTE, neutral);
			ctx->p_glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_flow[3]);
			ctx->p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
					       (GLsizei)w4, (GLsizei)h4,
					       GL_RGBA, GL_UNSIGNED_BYTE, neutral);

			ctx->p_glPixelStorei(GL_UNPACK_ROW_LENGTH, old_rl);
			ctx->p_glPixelStorei(GL_UNPACK_SKIP_PIXELS, old_sp);
			ctx->p_glPixelStorei(GL_UNPACK_SKIP_ROWS, old_sr);
			ctx->p_glPixelStorei(GL_UNPACK_ALIGNMENT, old_al);
			free(neutral);
		}
	}

	ctx->p_glBindTexture(GL_TEXTURE_2D, 0);
	{
		GLint old_fbo = 0;

		ctx->p_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
		ctx->p_glBindFramebuffer(GL_FRAMEBUFFER, ctx->gl_fbo);
		ctx->p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
					      GL_TEXTURE_2D, ctx->gl_tex_flow[3], 0);
		ctx->gl_fbo_ok = (ctx->p_glCheckFramebufferStatus(GL_FRAMEBUFFER) ==
				  GL_FRAMEBUFFER_COMPLETE);
		ctx->p_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fbo);
	}
	ctx->gl_w = ctx->width;
	ctx->gl_h = ctx->height;
	ctx->flow_w = w4;
	ctx->flow_h = h4;
	ctx->gl_flow_scale = scale;
	ctx->gl_uploaded_seq = 0;
}

void gc573_fg_native_gl_teardown(struct gc573_fg_native *ctx)
{
	if (!ctx)
		return;
	if (SDL_GL_GetCurrentContext()) {
		if (ctx->gl_tex_prev && ctx->p_glDeleteTextures)
			ctx->p_glDeleteTextures(1, &ctx->gl_tex_prev);
		if (ctx->gl_tex_curr && ctx->p_glDeleteTextures)
			ctx->p_glDeleteTextures(1, &ctx->gl_tex_curr);
		if (ctx->gl_tex_mv && ctx->p_glDeleteTextures)
			ctx->p_glDeleteTextures(1, &ctx->gl_tex_mv);
		if (ctx->gl_tex_luma_p[0] && ctx->p_glDeleteTextures)
			ctx->p_glDeleteTextures(3, ctx->gl_tex_luma_p);
		if (ctx->gl_tex_luma_c[0] && ctx->p_glDeleteTextures)
			ctx->p_glDeleteTextures(3, ctx->gl_tex_luma_c);
		if (ctx->gl_tex_flow[0] && ctx->p_glDeleteTextures)
			ctx->p_glDeleteTextures(5, ctx->gl_tex_flow);
		if (ctx->gl_fbo && ctx->p_glDeleteFramebuffers)
			ctx->p_glDeleteFramebuffers(1, &ctx->gl_fbo);
		if (ctx->gl_prog && ctx->p_glDeleteProgram)
			ctx->p_glDeleteProgram(ctx->gl_prog);
		if (ctx->gl_prog_luma && ctx->p_glDeleteProgram)
			ctx->p_glDeleteProgram(ctx->gl_prog_luma);
		if (ctx->gl_prog_down && ctx->p_glDeleteProgram)
			ctx->p_glDeleteProgram(ctx->gl_prog_down);
		if (ctx->gl_prog_flow && ctx->p_glDeleteProgram)
			ctx->p_glDeleteProgram(ctx->gl_prog_flow);
		if (ctx->gl_prog_smooth && ctx->p_glDeleteProgram)
			ctx->p_glDeleteProgram(ctx->gl_prog_smooth);
	}
	ctx->gl_tex_prev = 0;
	ctx->gl_tex_curr = 0;
	ctx->gl_tex_mv = 0;
	memset(ctx->gl_tex_luma_p, 0, sizeof(ctx->gl_tex_luma_p));
	memset(ctx->gl_tex_luma_c, 0, sizeof(ctx->gl_tex_luma_c));
	memset(ctx->gl_tex_flow, 0, sizeof(ctx->gl_tex_flow));
	ctx->gl_fbo = 0;
	ctx->gl_prog = 0;
	ctx->gl_prog_luma = 0;
	ctx->gl_prog_down = 0;
	ctx->gl_prog_flow = 0;
	ctx->gl_prog_smooth = 0;
	ctx->gl_w = 0;
	ctx->gl_h = 0;
	ctx->flow_w = 0;
	ctx->flow_h = 0;
	ctx->gl_uploaded_seq = 0;
	ctx->gl_fbo_ok = false;
	ctx->gl_ok = false;
	ctx->gl_tried = false;
}

static void fgn_draw_quad(struct gc573_fg_native *ctx, GLint a_pos, const GLfloat *quad)
{
	if (a_pos < 0)
		return;
	ctx->p_glEnableVertexAttribArray((GLuint)a_pos);
	ctx->p_glVertexAttribPointer((GLuint)a_pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
	ctx->p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	ctx->p_glDisableVertexAttribArray((GLuint)a_pos);
}

static void fgn_gpu_luma_pyramid(struct gc573_fg_native *ctx,
				 GLuint src_yuyv, GLuint dst_luma[3],
				 const GLfloat *quad)
{
	uint32_t w4 = ctx->flow_w, h4 = ctx->flow_h;
	uint32_t w8 = w4 / 2U > 16U ? w4 / 2U : 16U;
	uint32_t h8 = h4 / 2U > 16U ? h4 / 2U : 16U;
	uint32_t w16 = w8 / 2U > 8U ? w8 / 2U : 8U;
	uint32_t h16 = h8 / 2U > 8U ? h8 / 2U : 8U;

	/* Level 0: YUYV -> 1/4 luma */
	ctx->p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				      GL_TEXTURE_2D, dst_luma[0], 0);
	ctx->p_glViewport(0, 0, (GLsizei)w4, (GLsizei)h4);
	ctx->p_glUseProgram(ctx->gl_prog_luma);
	ctx->p_glActiveTexture(GL_TEXTURE0);
	ctx->p_glBindTexture(GL_TEXTURE_2D, src_yuyv);
	ctx->p_glUniform1i(ctx->gl_luma_u_tex, 0);
	ctx->p_glUniform2f(ctx->gl_luma_u_size,
			   (float)ctx->width, (float)ctx->height);
	fgn_draw_quad(ctx, ctx->gl_luma_a_pos, quad);

	/* Level 1: 1/4 -> 1/8 */
	ctx->p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				      GL_TEXTURE_2D, dst_luma[1], 0);
	ctx->p_glViewport(0, 0, (GLsizei)w8, (GLsizei)h8);
	ctx->p_glUseProgram(ctx->gl_prog_down);
	ctx->p_glBindTexture(GL_TEXTURE_2D, dst_luma[0]);
	ctx->p_glUniform1i(ctx->gl_down_u_tex, 0);
	ctx->p_glUniform2f(ctx->gl_down_u_texel,
			   1.0f / (float)w4, 1.0f / (float)h4);
	fgn_draw_quad(ctx, ctx->gl_down_a_pos, quad);

	/* Level 2: 1/8 -> 1/16 */
	ctx->p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				      GL_TEXTURE_2D, dst_luma[2], 0);
	ctx->p_glViewport(0, 0, (GLsizei)w16, (GLsizei)h16);
	ctx->p_glBindTexture(GL_TEXTURE_2D, dst_luma[1]);
	ctx->p_glUniform2f(ctx->gl_down_u_texel,
			   1.0f / (float)w8, 1.0f / (float)h8);
	fgn_draw_quad(ctx, ctx->gl_down_a_pos, quad);
}

static void fgn_gpu_flow_level(struct gc573_fg_native *ctx,
			       GLuint luma_p, GLuint luma_c,
			       GLuint flow_prior, GLuint flow_out,
			       uint32_t lvl_w, uint32_t lvl_h,
			       float has_prior, float radius,
			       const GLfloat *quad)
{
	ctx->p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				      GL_TEXTURE_2D, flow_out, 0);
	ctx->p_glViewport(0, 0, (GLsizei)lvl_w, (GLsizei)lvl_h);
	ctx->p_glUseProgram(ctx->gl_prog_flow);

	ctx->p_glActiveTexture(GL_TEXTURE0);
	ctx->p_glBindTexture(GL_TEXTURE_2D, luma_p);
	ctx->p_glUniform1i(ctx->gl_flow_u_luma_p, 0);

	ctx->p_glActiveTexture(GL_TEXTURE1);
	ctx->p_glBindTexture(GL_TEXTURE_2D, luma_c);
	ctx->p_glUniform1i(ctx->gl_flow_u_luma_c, 1);

	ctx->p_glActiveTexture(GL_TEXTURE2);
	ctx->p_glBindTexture(GL_TEXTURE_2D, flow_prior ? flow_prior : luma_c);
	ctx->p_glUniform1i(ctx->gl_flow_u_flow_in, 2);

	ctx->p_glUniform2f(ctx->gl_flow_u_size,
			   (float)ctx->width, (float)ctx->height);
	ctx->p_glUniform2f(ctx->gl_flow_u_lvl_size,
			   (float)lvl_w, (float)lvl_h);
	ctx->p_glUniform1f(ctx->gl_flow_u_has_prior, has_prior);
	ctx->p_glUniform1f(ctx->gl_flow_u_radius, radius);
	fgn_draw_quad(ctx, ctx->gl_flow_a_pos, quad);
}

static void fgn_gpu_compute_flow(struct gc573_fg_native *ctx,
				 bool full_luma_rebuild,
				 const GLfloat *quad)
{
	GLint old_fbo = 0, old_vbo = 0;
	GLboolean was_blend = ctx->p_glIsEnabled(GL_BLEND);
	GLboolean was_scissor = ctx->p_glIsEnabled(GL_SCISSOR_TEST);
	uint32_t w4 = ctx->flow_w, h4 = ctx->flow_h;
	uint32_t w8 = w4 / 2U > 16U ? w4 / 2U : 16U;
	uint32_t h8 = h4 / 2U > 16U ? h4 / 2U : 16U;
	uint32_t w16 = w8 / 2U > 8U ? w8 / 2U : 8U;
	uint32_t h16 = h8 / 2U > 8U ? h8 / 2U : 8U;

	if (!ctx->gl_fbo_ok)
		return;

	ctx->p_glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
	if (ctx->p_glBindBuffer) {
		ctx->p_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_vbo);
		if (old_vbo != 0)
			ctx->p_glBindBuffer(GL_ARRAY_BUFFER, 0);
	}
	if (was_blend) ctx->p_glDisable(GL_BLEND);
	if (was_scissor) ctx->p_glDisable(GL_SCISSOR_TEST);

	ctx->p_glBindFramebuffer(GL_FRAMEBUFFER, ctx->gl_fbo);

	if (full_luma_rebuild)
		fgn_gpu_luma_pyramid(ctx, ctx->gl_tex_prev, ctx->gl_tex_luma_p, quad);
	fgn_gpu_luma_pyramid(ctx, ctx->gl_tex_curr, ctx->gl_tex_luma_c, quad);

	/* Coarse (1/16) -> Mid (1/8) -> Fine (1/4) Optical Flow */
	float r0 = ctx->perf_mode ? 2.0f : 3.0f;
	float r1 = ctx->perf_mode ? 1.0f : 2.0f;
	float r2 = ctx->perf_mode ? 0.5f : 1.0f;
	fgn_gpu_flow_level(ctx, ctx->gl_tex_luma_p[2], ctx->gl_tex_luma_c[2],
			   0, ctx->gl_tex_flow[0], w16, h16, 0.0f, r0, quad);
	fgn_gpu_flow_level(ctx, ctx->gl_tex_luma_p[1], ctx->gl_tex_luma_c[1],
			   ctx->gl_tex_flow[0], ctx->gl_tex_flow[1], w8, h8, 1.0f, r1, quad);
	fgn_gpu_flow_level(ctx, ctx->gl_tex_luma_p[0], ctx->gl_tex_luma_c[0],
			   ctx->gl_tex_flow[1], ctx->gl_tex_flow[2], w4, h4, 1.0f, r2, quad);

	/* Bilateral edge-preserving smooth + temporal static HUD lock (ping-pong [3] <-> [4]) */
	{
		GLuint prev_hist = ctx->gl_tex_flow[4];
		ctx->gl_tex_flow[4] = ctx->gl_tex_flow[3];
		ctx->gl_tex_flow[3] = prev_hist;
	}
	ctx->p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				      GL_TEXTURE_2D, ctx->gl_tex_flow[3], 0);
	ctx->p_glViewport(0, 0, (GLsizei)w4, (GLsizei)h4);
	ctx->p_glUseProgram(ctx->gl_prog_smooth);

	ctx->p_glActiveTexture(GL_TEXTURE0);
	ctx->p_glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_flow[2]);
	ctx->p_glUniform1i(ctx->gl_smooth_u_flow, 0);

	ctx->p_glActiveTexture(GL_TEXTURE1);
	ctx->p_glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_luma_p[0]);
	ctx->p_glUniform1i(ctx->gl_smooth_u_luma_p, 1);

	ctx->p_glActiveTexture(GL_TEXTURE2);
	ctx->p_glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_luma_c[0]);
	ctx->p_glUniform1i(ctx->gl_smooth_u_luma_c, 2);

	ctx->p_glActiveTexture(GL_TEXTURE3);
	ctx->p_glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_flow[4]);
	ctx->p_glUniform1i(ctx->gl_smooth_u_prev_flow, 3);

	ctx->p_glUniform2f(ctx->gl_smooth_u_lvl_size,
			   (float)w4, (float)h4);
	fgn_draw_quad(ctx, ctx->gl_smooth_a_pos, quad);

	ctx->p_glActiveTexture(GL_TEXTURE3);
	ctx->p_glBindTexture(GL_TEXTURE_2D, 0);
	ctx->p_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fbo);
	if (was_blend) ctx->p_glEnable(GL_BLEND);
	if (was_scissor) ctx->p_glEnable(GL_SCISSOR_TEST);
	if (ctx->p_glBindBuffer && old_vbo != 0)
		ctx->p_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_vbo);
}

bool gc573_fg_native_gl_render(struct gc573_fg_native *ctx,
			       float t,
			       int dst_x, int dst_y,
			       int dst_w, int dst_h,
			       int win_w, int win_h)
{
	static const GLfloat quad[8] = {
		-1.0f, -1.0f,
		 1.0f, -1.0f,
		-1.0f,  1.0f,
		 1.0f,  1.0f,
	};

	if (!gc573_fg_native_ready(ctx) || dst_w <= 0 || dst_h <= 0)
		return false;
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;
	if (!fgn_gl_init(ctx))
		return false;

	float want_scale = (ctx->flow_scale >= 0.5f && ctx->flow_scale <= 1.0f)
			   ? ctx->flow_scale : 1.0f;
	if (ctx->gl_w != ctx->width || ctx->gl_h != ctx->height ||
	    fabsf(ctx->gl_flow_scale - want_scale) > 0.02f)
		fgn_gl_alloc_textures(ctx);

	/* Upload ONLY when a new real capture frame arrived (60 Hz) */
	if (ctx->gl_uploaded_seq != ctx->frame_seq) {
		GLint old_row_len = 0, old_skip_px = 0, old_skip_rows = 0, old_align = 4;
		bool seq_step = (ctx->gl_uploaded_seq + 1 == ctx->frame_seq);

		ctx->p_glGetIntegerv(GL_UNPACK_ROW_LENGTH, &old_row_len);
		ctx->p_glGetIntegerv(GL_UNPACK_SKIP_PIXELS, &old_skip_px);
		ctx->p_glGetIntegerv(GL_UNPACK_SKIP_ROWS, &old_skip_rows);
		ctx->p_glGetIntegerv(GL_UNPACK_ALIGNMENT, &old_align);

		ctx->p_glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
		ctx->p_glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
		ctx->p_glPixelStorei(GL_UNPACK_SKIP_ROWS, 0);
		ctx->p_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

		if (seq_step) {
			/* Swap prev <-> curr YUYV + luma pyramids (only upload ONE frame) */
			GLuint tmp = ctx->gl_tex_prev;
			ctx->gl_tex_prev = ctx->gl_tex_curr;
			ctx->gl_tex_curr = tmp;
			for (int lvl = 0; lvl < 3; lvl++) {
				GLuint tl = ctx->gl_tex_luma_p[lvl];
				ctx->gl_tex_luma_p[lvl] = ctx->gl_tex_luma_c[lvl];
				ctx->gl_tex_luma_c[lvl] = tl;
			}
		} else {
			ctx->p_glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_prev);
			ctx->p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
					       (GLsizei)(ctx->width / 2U), (GLsizei)ctx->height,
					       GL_RGBA, GL_UNSIGNED_BYTE, ctx->prev_yuyv);
		}
		ctx->p_glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_curr);
		ctx->p_glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
				       (GLsizei)(ctx->width / 2U), (GLsizei)ctx->height,
				       GL_RGBA, GL_UNSIGNED_BYTE, ctx->curr_yuyv);

		ctx->p_glPixelStorei(GL_UNPACK_ROW_LENGTH, old_row_len);
		ctx->p_glPixelStorei(GL_UNPACK_SKIP_PIXELS, old_skip_px);
		ctx->p_glPixelStorei(GL_UNPACK_SKIP_ROWS, old_skip_rows);
		ctx->p_glPixelStorei(GL_UNPACK_ALIGNMENT, old_align);

		/* Run Multi-Scale Dense Optical Flow on GPU (0 ms CPU work) */
		fgn_gpu_compute_flow(ctx, !seq_step, quad);

		ctx->gl_uploaded_seq = ctx->frame_seq;
	}

	ctx->p_glViewport(dst_x, win_h - dst_y - dst_h, dst_w, dst_h);
	ctx->p_glUseProgram(ctx->gl_prog);

	ctx->p_glActiveTexture(GL_TEXTURE0);
	ctx->p_glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_prev);
	ctx->p_glUniform1i(ctx->gl_u_prev, 0);

	ctx->p_glActiveTexture(GL_TEXTURE1);
	ctx->p_glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_curr);
	ctx->p_glUniform1i(ctx->gl_u_curr, 1);

	ctx->p_glActiveTexture(GL_TEXTURE2);
	ctx->p_glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_flow[3]);
	ctx->p_glUniform1i(ctx->gl_u_mv, 2);

	ctx->p_glActiveTexture(GL_TEXTURE3);
	ctx->p_glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_luma_c[0]);
	ctx->p_glUniform1i(ctx->gl_u_luma_c, 3);

	ctx->p_glUniform1f(ctx->gl_u_t, t);
	ctx->p_glUniform2f(ctx->gl_u_size, (float)ctx->width, (float)ctx->height);
	ctx->p_glUniform2f(ctx->gl_u_flow_size, (float)ctx->flow_w, (float)ctx->flow_h);

	{
		GLint old_vbo = 0;
		GLboolean was_depth = ctx->p_glIsEnabled(GL_DEPTH_TEST);
		GLboolean was_cull  = ctx->p_glIsEnabled(GL_CULL_FACE);

		if (was_depth) ctx->p_glDisable(GL_DEPTH_TEST);
		if (was_cull)  ctx->p_glDisable(GL_CULL_FACE);
		if (ctx->p_glBindBuffer) {
			ctx->p_glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &old_vbo);
			if (old_vbo != 0)
				ctx->p_glBindBuffer(GL_ARRAY_BUFFER, 0);
		}
		ctx->p_glEnableVertexAttribArray((GLuint)ctx->gl_a_pos);
		ctx->p_glVertexAttribPointer((GLuint)ctx->gl_a_pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
		ctx->p_glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		ctx->p_glDisableVertexAttribArray((GLuint)ctx->gl_a_pos);
		if (ctx->p_glBindBuffer && old_vbo != 0)
			ctx->p_glBindBuffer(GL_ARRAY_BUFFER, (GLuint)old_vbo);
		if (was_depth) ctx->p_glEnable(GL_DEPTH_TEST);
		if (was_cull)  ctx->p_glEnable(GL_CULL_FACE);
	}

	ctx->p_glUseProgram(0);
	ctx->p_glActiveTexture(GL_TEXTURE3);
	ctx->p_glBindTexture(GL_TEXTURE_2D, 0);
	ctx->p_glActiveTexture(GL_TEXTURE2);
	ctx->p_glBindTexture(GL_TEXTURE_2D, 0);
	ctx->p_glActiveTexture(GL_TEXTURE1);
	ctx->p_glBindTexture(GL_TEXTURE_2D, 0);
	ctx->p_glActiveTexture(GL_TEXTURE0);
	ctx->p_glBindTexture(GL_TEXTURE_2D, 0);
	ctx->p_glViewport(0, 0, win_w, win_h);

	return true;
}
