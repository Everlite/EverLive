#pragma once

#include "layer.h"

/* Frame generation proper: FidelityFX Optical Flow (colour in, 8x8 block motion + scene change
 * detector out) followed by the layer's own interpolation shader. One instance per swapchain; the
 * pipelines are shared per device and built on first use. */

struct afmf_framegen;

/* Whether the interpolated frame can be written straight into a swapchain image of this format
 * (the image then needs STORAGE usage): the format takes storage writes, and either has a SPIR-V
 * image format or the application enabled shaderStorageImageWriteWithoutFormat. */
bool afmf_framegen_can_write_direct(const struct afmf_device *dev, VkFormat swapchain_format);

/* NULL when the swapchain format has no interpolation variant or the device lacks a required
 * format feature; the caller then falls back to repeating frames. `images` are the swapchain's:
 * with `direct_output` (STORAGE usage on them) the interpolator writes into them instead of an
 * internal image copied at record time; with `direct_ingest` (SAMPLED usage) the frame is read
 * from them straight into the colour ring and the luma instead of copied first. */
struct afmf_framegen *afmf_framegen_create(struct afmf_device *dev, VkFormat swapchain_format,
                                           VkExtent2D extent, uint32_t slots,
                                           const VkImage *images, uint32_t image_count,
                                           bool direct_output, bool direct_ingest);

/* True when the interpolator writes the target itself: the caller hands it in GENERAL layout. */
bool afmf_framegen_direct(const struct afmf_framegen *fg);

/* Whether the frame can be read straight from a swapchain image of this format into the colour
 * ring and the luma in one pass (the image then needs SAMPLED usage): the swapchain's format is
 * the ring's own (no sRGB decode on the way) and the ring takes storage writes. */
bool afmf_framegen_can_ingest_direct(const struct afmf_device *dev, VkFormat swapchain_format);

/* True when the ingest samples the current image itself: the caller hands it in GENERAL layout
 * instead of TRANSFER_SRC. */
bool afmf_framegen_ingest_direct(const struct afmf_framegen *fg);

void afmf_framegen_destroy(struct afmf_device *dev, struct afmf_framegen *fg);

/* The queue family afmf_framegen_record's command buffers belong to, once the caller knows it:
 * the fixed part of a frame is then recorded once per (slot, parity, companion) into secondary
 * command buffers of that family and only executed from then on. Without this call (or when it
 * fails) every frame is recorded in full. */
void afmf_framegen_set_family(struct afmf_device *dev, struct afmf_framegen *fg, uint32_t family);

/* Records, into a command buffer on a compute-capable queue:
 *   1. a copy of `current` (a swapchain image in TRANSFER_SRC_OPTIMAL) into the colour ring,
 *   2. the luma pyramid and scene change detector on the new frame, and, when `companion_count > 0`,
 *      the block search,
 *   3. when `companion_count > 0`: the interpolated frames between the previous and new frame at fractions.
 * Without companions the frame only enters the history (about a fifth of the cost).
 * `slot` selects the constants region so `slots` command buffers can be in flight. */
void afmf_framegen_record(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                          uint32_t slot, VkImage current, uint32_t current_index,
                          const uint32_t *companion_images, uint32_t companion_count,
                          const VkImage *sc_images);

/* Pyramid levels the search uses from now on, between 5 and what the swapchain was set up with;
 * fewer levels cost less and reach shorter motion. The governor lowers it under GPU contention. */
void afmf_framegen_set_levels(struct afmf_framegen *fg, uint32_t levels);
uint32_t afmf_framegen_max_levels(const struct afmf_framegen *fg);

/* Debug dumps (AFMF_DUMP_DIR): afmf_framegen_record also copies the generated frame, the flow
 * and the luma into a readback buffer while `afmf_framegen_dump_pending` is true (the first
 * four companions from frame 8 on, after the scene change detector's warm-up);
 * `afmf_framegen_dump_recorded` says whether the command buffer just recorded did, and once
 * its submission has completed afmf_framegen_dump_write stores <dir>/afmf_generated_<frame>.ppm
 * and <dir>/afmf_flow_<frame>.txt ("vx vy" per block, prev = cur + v). 8-bit variants only. */
/* Rearms the four-frame dump when AFMF_DUMP_DIR/dump-now exists, so a dump can be taken from
 * whatever is on screen instead of the frames that follow a swapchain's creation. */
void afmf_framegen_dump_arm(struct afmf_framegen *fg);
bool afmf_framegen_dump_pending(const struct afmf_framegen *fg);
bool afmf_framegen_dump_recorded(const struct afmf_framegen *fg);
void afmf_framegen_dump_write(struct afmf_device *dev, struct afmf_framegen *fg);

/* Compiles the per-device pipelines on a thread of their own, from vkCreateDevice; the first
 * swapchain joins it. Frees them from vkDestroyDevice. */
void afmf_framegen_pipelines_prepare(struct afmf_device *dev);
void afmf_framegen_pipelines_destroy(struct afmf_device *dev);
