/* Frame generation: one frame into the primary command buffer. The fixed part (flow and
 * interpolation) is recorded once into a secondary command buffer per (slot, parity, companion)
 * and executed from here, which keeps only what changes per frame: the copies from and to the
 * swapchain images, and the dump. */

#include "framegen_internal.h"

/* Executes the pre-recorded fixed part for (slot, p, companion), recording it first when it has
 * never run or the search levels changed since; the slot's fence has been waited on by the
 * caller, so nothing of this slot is in flight. Records inline when there are no secondaries. */
static void execute_flow(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                         uint32_t slot, uint32_t p, uint32_t levels, bool companion)
{
    struct secondary *sec =
        fg->secondaries != NULL ? &fg->secondaries[(slot * 2u + p) * 2u + (companion ? 1u : 0u)] : NULL;
    if (sec == NULL) {
        afmf_framegen_record_flow(dev, fg, cmd, slot, p, levels, companion);
        return;
    }
    bool profiling = fg->queries != VK_NULL_HANDLE;
    uint32_t before = profiling ? fg->query_count[slot] : 0;
    if (!sec->recorded || sec->levels != levels) {
        /* The marks it writes land at fixed query indices: the primary always writes the same
         * two before it (opening, ingest), so a replay only needs the stage list. */
        VkCommandBufferInheritanceInfo inherit = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO};
        VkCommandBufferBeginInfo begin = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .pInheritanceInfo = &inherit};
        sec->recorded = false;
        if (dev->fns.begin_command_buffer(sec->cmd, &begin) == VK_SUCCESS) {
            afmf_framegen_record_flow(dev, fg, sec->cmd, slot, p, levels, companion);
            sec->recorded = dev->fns.end_command_buffer(sec->cmd) == VK_SUCCESS;
        }
        if (!sec->recorded) {
            if (profiling)
                fg->query_count[slot] = before;
            afmf_framegen_record_flow(dev, fg, cmd, slot, p, levels, companion);
            return;
        }
        sec->levels = levels;
        sec->stage_count = profiling ? fg->query_count[slot] - before : 0;
        if (sec->stage_count > 0)
            memcpy(sec->stages, fg->query_stage + (size_t)slot * AFMF_PROFILE_QUERIES + before,
                   sec->stage_count);
    } else if (profiling && before + sec->stage_count <= AFMF_PROFILE_QUERIES) {
        memcpy(fg->query_stage + (size_t)slot * AFMF_PROFILE_QUERIES + before, sec->stages,
               sec->stage_count);
        fg->query_count[slot] = before + sec->stage_count;
    }
    dev->fns.cmd_execute_commands(cmd, 1, &sec->cmd);
}

/* The generated frame, the flow and the luma into the readback buffer, in the order
 * afmf_framegen_dump_write reads them back. */
static void record_dump(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                        uint32_t p, VkImage target)
{
    uint32_t out_w = fg->extent.width, out_h = fg->extent.height;
    VkBufferImageCopy regions[2] = {
        {
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {out_w, out_h, 1},
        },
        {
            .bufferOffset = (VkDeviceSize)out_w * out_h * 4u,
            .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
            .imageExtent = {fg->flow_size[0].width, fg->flow_size[0].height, 1},
        },
    };
    VkDeviceSize flow_bytes = (VkDeviceSize)fg->flow_size[0].width * fg->flow_size[0].height * 4u;
    VkBufferImageCopy raw_region = regions[1];
    raw_region.bufferOffset += flow_bytes;
    VkBufferImageCopy scd_region = {
        .bufferOffset = (VkDeviceSize)out_w * out_h * 4u + 2u * flow_bytes,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {AFMF_SCD_SLOTS, 1, 1},
    };
    VkBufferImageCopy luma_region = {
        .bufferOffset = scd_region.bufferOffset + AFMF_SCD_SLOTS * 4u,
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {fg->luma_size[0].width, fg->luma_size[0].height, 1},
    };
    VkBufferImageCopy luma_prev_region = luma_region;
    luma_prev_region.bufferOffset += (VkDeviceSize)fg->luma_size[0].width * fg->luma_size[0].height;
    if (fg->direct)
        afmf_sync(dev, cmd); /* the copy reads what the dispatch just wrote */
    dev->fns.cmd_copy_image_to_buffer(cmd, fg->direct ? target : fg->output.image,
                                      VK_IMAGE_LAYOUT_GENERAL, fg->dump_buffer, 1, &regions[0]);
    dev->fns.cmd_copy_image_to_buffer(cmd, fg->flow_out.image, VK_IMAGE_LAYOUT_GENERAL,
                                      fg->dump_buffer, 1, &regions[1]);
    dev->fns.cmd_copy_image_to_buffer(cmd, fg->flow[afmf_flow_parity_a(p, 0)][0].image,
                                      VK_IMAGE_LAYOUT_GENERAL, fg->dump_buffer, 1, &raw_region);
    dev->fns.cmd_copy_image_to_buffer(cmd, fg->scd_output.image, VK_IMAGE_LAYOUT_GENERAL,
                                      fg->dump_buffer, 1, &scd_region);
    dev->fns.cmd_copy_image_to_buffer(cmd, fg->luma[p][0].image, VK_IMAGE_LAYOUT_GENERAL,
                                      fg->dump_buffer, 1, &luma_region);
    dev->fns.cmd_copy_image_to_buffer(cmd, fg->luma[1u - p][0].image, VK_IMAGE_LAYOUT_GENERAL,
                                      fg->dump_buffer, 1, &luma_prev_region);
    fg->dump_recorded = true;
    fg->dump_frame = fg->frame_index;
}

void afmf_framegen_record(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                          uint32_t slot, VkImage current, uint32_t current_index,
                          const uint32_t *companion_images, uint32_t companion_count,
                          const VkImage *sc_images)
{
    const struct afmf_framegen_pipelines *pl = dev->framegen_pipelines;
    uint32_t levels = fg->levels;
    uint32_t p = fg->frame_index & 1u;
    uint32_t w = fg->of_extent.width, h = fg->of_extent.height; /* optical flow dimensions */
    bool companion = companion_count > 0;

    /* The direct paths keep one view per swapchain image: an index past them is a broken
     * caller, not a case to fall back from. */
    if (fg->ingest_direct && current_index >= fg->target_count) {
        AFMF_ERR("frame %u outside the %u swapchain images: nothing recorded",
                 current_index, fg->target_count);
        return;
    }

    if (!fg->initialized)
        afmf_framegen_initialize(dev, fg, cmd);
    afmf_framegen_write_constants(fg, slot, levels);

    /* The previous frame's work on this queue may still be reading the ring image about to be
     * overwritten: order it before anything below. */
    afmf_sync(dev, cmd);
    afmf_framegen_profiler_begin(dev, fg, cmd, slot);

    /* 1. The new frame into the colour ring: one pass that also writes its luma (direct ingest,
     *    the swapchain image sampled in GENERAL), or a copy, then downscaled for the flow when
     *    in performance mode. The direct pass writes the luma at the flow's own size, so
     *    performance mode costs it a read of the frame and nothing else; the copy path has to
     *    downscale the colour and take the luma from it, two more passes over the frame. */
    if (fg->ingest_direct) {
        struct ingest_push push = {
            .size = {(int32_t)fg->extent.width, (int32_t)fg->extent.height},
            .transfer_function = (int32_t)fg->transfer_function,
            .min_luminance = fg->min_luminance,
            .max_luminance = fg->max_luminance,
            .luma_half = fg->flow_scale > 1 ? 1 : 0,
        };
        uint32_t ingest_w = fg->flow_scale > 1 ? w : fg->extent.width;
        uint32_t ingest_h = fg->flow_scale > 1 ? h : fg->extent.height;
        dev->fns.cmd_push_constants(cmd, pl->layouts[PASS_INGEST], VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                    (uint32_t)sizeof push, &push);
        afmf_dispatch(dev, cmd, pl->ingest[fg->ingest_variant], pl->layouts[PASS_INGEST],
                      fg->ingest_sets[p * fg->target_count + current_index], NULL, 0,
                      (ingest_w + 7) / 8, (ingest_h + 7) / 8, 1);
    } else {
        afmf_framegen_copy_whole(dev, cmd, current, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                 fg->color[p].image, VK_IMAGE_LAYOUT_GENERAL, fg->extent);
        afmf_sync(dev, cmd);
    }
    if (fg->flow_scale > 1 && !fg->ingest_direct) {
        struct downsample_push push = {
            .size = {(int32_t)w, (int32_t)h},
            .source = {(int32_t)fg->extent.width, (int32_t)fg->extent.height},
        };
        dev->fns.cmd_push_constants(cmd, pl->layouts[PASS_DOWNSAMPLE], VK_SHADER_STAGE_COMPUTE_BIT,
                                    0, (uint32_t)sizeof push, &push);
        afmf_dispatch(dev, cmd, pl->downsample[fg->half], pl->layouts[PASS_DOWNSAMPLE],
                      fg->sets[p][SET_DOWNSAMPLE], NULL, 0, (w + 7) / 8, (h + 7) / 8, 1);
    }
    afmf_framegen_profiler_mark(dev, fg, cmd, slot, STAGE_INGEST);

    /* 2. Optical flow: the pre-recorded part. */
    execute_flow(dev, fg, cmd, slot, p, levels, companion);

    /* 3. The frames in between, into the targets: written by the interpolator itself (direct
     *    output, target in GENERAL) or copied from fg->output (target in TRANSFER_DST). */
    for (uint32_t k = 0; k < companion_count; k++) {
        uint32_t target_index = companion_images[k];
        VkImage target = sc_images[target_index];
        float t = 0.5f;
        if (companion_count == 1) {
            t = 0.5f;
        } else if (companion_count == 2) {
            t = (k == 0) ? 0.333333f : 0.666667f;
        } else if (companion_count >= 3) {
            t = (k == 0) ? 0.25f : ((k == 1) ? 0.50f : 0.75f);
        }

        if (fg->direct) {
            afmf_framegen_record_interpolate(dev, fg, cmd, slot,
                                             fg->direct_sets[p * fg->target_count + target_index],
                                             fg->direct_variant, t);
        } else {
            afmf_framegen_record_interpolate(dev, fg, cmd, slot,
                                             fg->sets[p][SET_INTERPOLATE],
                                             fg->variant, t);
            afmf_sync(dev, cmd);
            afmf_framegen_copy_whole(dev, cmd, fg->output.image, VK_IMAGE_LAYOUT_GENERAL, target,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fg->extent);
            afmf_framegen_profiler_mark(dev, fg, cmd, slot, STAGE_OUTPUT);
        }
        if (afmf_framegen_dump_pending(fg) && k == 0)
            record_dump(dev, fg, cmd, p, target);
    }

    fg->frame_index++;
}
