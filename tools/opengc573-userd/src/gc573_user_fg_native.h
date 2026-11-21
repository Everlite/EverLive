/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * gc573_user_fg_native.h — Built-in GPL-2.0 CPU+YUYV Frame Generation
 *
 * Pure open-source motion-compensated frame interpolation prototype:
 *   1. Downsamples YUYV luma (4x4) for fast block-matching motion estimation
 *      using neighbor prediction + diamond search.
 *   2. Applies a Static HUD Lock so motionless UI elements (bars, text,
 *      minimap) are preserved without optical-flow ghosting.
 *   3. Synthesizes intermediate YUYV frames at t in (0, 1) using
 *      bidirectional motion-compensated warping with occlusion fallback.
 */
#ifndef GC573_USER_FG_NATIVE_H
#define GC573_USER_FG_NATIVE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gc573_fg_native;

struct gc573_fg_native *gc573_fg_native_create(void);
void gc573_fg_native_destroy(struct gc573_fg_native *ctx);
void gc573_fg_native_reset(struct gc573_fg_native *ctx);
void gc573_fg_native_set_params(struct gc573_fg_native *ctx,
				float flow_scale,
				bool perf_mode);

/*
 * Feed a newly captured real YUYV frame (width * height * 2 bytes).
 * Updates internal prev/curr buffers and computes block motion vectors
 * once both prev and curr are available.
 */
bool gc573_fg_native_push(struct gc573_fg_native *ctx,
			  const uint8_t *yuyv,
			  uint32_t width,
			  uint32_t height,
			  uint32_t stride_bytes);

bool gc573_fg_native_ready(const struct gc573_fg_native *ctx);

/*
 * Pointer to the previous real YUYV frame (t=0) so presentation always
 * advances strictly forward in time: prev -> synth(0.5) -> curr.
 */
const uint8_t *gc573_fg_native_prev(const struct gc573_fg_native *ctx);

/*
 * Synthesize an interpolated YUYV frame at normalized time t in (0.0, 1.0)
 * between prev (t=0) and curr (t=1) into out_yuyv (must hold width*height*2 bytes).
 */
bool gc573_fg_native_synthesize(struct gc573_fg_native *ctx,
				float t,
				uint8_t *out_yuyv,
				uint32_t out_stride_bytes);

/*
 * Fast OpenGL GPU interpolation path (zero per-synthetic-frame texture upload).
 * Uploads only real frames + the tiny motion-vector grid (~14 KB at 1440p)
 * and renders any t in [0.0, 1.0] directly on the GPU.
 */
bool gc573_fg_native_gl_render(struct gc573_fg_native *ctx,
			       float t,
			       int dst_x, int dst_y,
			       int dst_w, int dst_h,
			       int win_w, int win_h);

void gc573_fg_native_gl_teardown(struct gc573_fg_native *ctx);

#ifdef __cplusplus
}
#endif

#endif /* GC573_USER_FG_NATIVE_H */
