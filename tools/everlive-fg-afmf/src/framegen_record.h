#pragma once

/* Frame generation: what the recording files share. The helpers every pass goes through are
 * inline here so no translation unit boundary sits in the per-frame recording. Included from
 * framegen_internal.h, after struct afmf_framegen. */

/* framegen_record.c */
void afmf_framegen_initialize(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd);
void afmf_framegen_write_constants(struct afmf_framegen *fg, uint32_t slot, uint32_t level_count);
void afmf_framegen_copy_whole(struct afmf_device *dev, VkCommandBuffer cmd, VkImage src,
                              VkImageLayout src_layout, VkImage dst, VkImageLayout dst_layout,
                              VkExtent2D extent);
void afmf_framegen_record_interpolate(struct afmf_device *dev, struct afmf_framegen *fg,
                                      VkCommandBuffer cmd, uint32_t slot, VkDescriptorSet set,
                                      enum variant variant, float timestep);
void afmf_framegen_record_flow(struct afmf_device *dev, struct afmf_framegen *fg, VkCommandBuffer cmd,
                               uint32_t slot, uint32_t p, uint32_t levels, bool companion);

/* Which of the two flow textures the search at `level` writes, for a frame of parity `p`: the
 * SDK alternates per frame and per level so that each level's output is the next level's input. */
static inline uint32_t afmf_flow_parity_a(uint32_t p, uint32_t level)
{
    return (p != (level & 1u)) ? 1u : 0u;
}

static inline uint32_t afmf_cb_offset(const struct afmf_framegen *fg, uint32_t slot, uint32_t index)
{
    return (slot * AFMF_CB_PER_SLOT + index) * fg->cb_stride;
}

/* Everything before this point is visible to everything after it, for compute and transfer. */
static inline void afmf_sync(struct afmf_device *dev, VkCommandBuffer cmd)
{
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                         VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    dev->fns.cmd_pipeline_barrier(cmd, stages, stages, 0, 1, &barrier, 0, NULL, 0, NULL);
}

/* Between two compute passes only: no transfer scopes, so the driver need not flush the caches a
 * copy engine would read. */
static inline void afmf_sync_compute(struct afmf_device *dev, VkCommandBuffer cmd)
{
    VkMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
    };
    dev->fns.cmd_pipeline_barrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &barrier, 0, NULL, 0,
                                  NULL);
}

/* Records one compute pass. `barrier` false lets the next pass start without waiting: only for
 * passes that neither read nor overwrite each other's outputs. */
static inline void afmf_dispatch_pass(struct afmf_device *dev, VkCommandBuffer cmd, VkPipeline pipeline,
                                      VkPipelineLayout layout, VkDescriptorSet set,
                                      const uint32_t *offsets, uint32_t offset_count, uint32_t x,
                                      uint32_t y, uint32_t z, bool barrier)
{
    dev->fns.cmd_bind_pipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
    dev->fns.cmd_bind_descriptor_sets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set,
                                      offset_count, offsets);
    dev->fns.cmd_dispatch(cmd, x, y, z);
    if (barrier)
        afmf_sync_compute(dev, cmd);
}

static inline void afmf_dispatch(struct afmf_device *dev, VkCommandBuffer cmd, VkPipeline pipeline,
                                 VkPipelineLayout layout, VkDescriptorSet set, const uint32_t *offsets,
                                 uint32_t offset_count, uint32_t x, uint32_t y, uint32_t z)
{
    afmf_dispatch_pass(dev, cmd, pipeline, layout, set, offsets, offset_count, x, y, z, true);
}
