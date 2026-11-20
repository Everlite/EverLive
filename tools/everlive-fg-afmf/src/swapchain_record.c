/* One frame's command buffer: the swapchain images through the layouts the work needs, around
 * the frame generation (framegen) or the plain copy of the history image. */

#include "swapchain_internal.h"

static void image_barrier(const struct afmf_device *dev, VkCommandBuffer cmd, VkImage image,
                          VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                          VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                          VkPipelineStageFlags dst_stage)
{
    VkImageMemoryBarrier barrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    dev->fns.cmd_pipeline_barrier(cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &barrier);
}

static void copy_whole(const struct afmf_device *dev, VkCommandBuffer cmd, VkImage src, VkImage dst,
                       VkExtent2D extent)
{
    VkImageCopy region = {
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .extent = {extent.width, extent.height, 1},
    };
    dev->fns.cmd_copy_image(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
}

/* Records the companion(s) for image i (frame N+1) into companion_images, and remembers frame N+1 for next
 * time. With interpolation: the optical flow and the interpolated frame(s) (framegen.c). Without it:
 * a copy of the history image into companion_images. Image i arrives in PRESENT_SRC (the application's
 * obligation) and is handed back in PRESENT_SRC. */
VkResult afmf_sc_record_frame(struct afmf_device *dev, struct afmf_swapchain *sc, VkCommandBuffer cmd,
                              uint32_t slot, uint32_t i, const uint32_t *companion_images, uint32_t companion_count)
{
    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VkResult res = dev->fns.begin_command_buffer(cmd, &begin);
    if (res != VK_SUCCESS)
        return res;

    const VkPipelineStageFlags top = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkPipelineStageFlags bottom = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    if (sc->fg != NULL) {
        /* The target is written by a copy (TRANSFER_DST) or, with direct output, by the
         * interpolator's stores (GENERAL). */
        bool direct = afmf_framegen_direct(sc->fg);
        bool ingest = afmf_framegen_ingest_direct(sc->fg);
        const VkPipelineStageFlags compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        VkImageLayout written = direct ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        VkAccessFlags write = direct ? VK_ACCESS_SHADER_WRITE_BIT : VK_ACCESS_TRANSFER_WRITE_BIT;
        VkPipelineStageFlags writer = direct ? compute : transfer;
        /* The current image is sampled by the ingest pass (GENERAL) or read by a copy. */
        VkImageLayout read = ingest ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VkAccessFlags read_access = ingest ? VK_ACCESS_SHADER_READ_BIT : VK_ACCESS_TRANSFER_READ_BIT;
        VkPipelineStageFlags reader = ingest ? compute : transfer;
        image_barrier(dev, cmd, sc->images[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, read, 0, read_access,
                      top, reader);
        for (uint32_t k = 0; k < companion_count; k++) {
            uint32_t j = companion_images[k];
            image_barrier(dev, cmd, sc->images[j], VK_IMAGE_LAYOUT_UNDEFINED, written, 0, write, top,
                          writer);
        }
        afmf_framegen_record(dev, sc->fg, cmd, slot, sc->images[i], i,
                             companion_images, companion_count, sc->images);
        for (uint32_t k = 0; k < companion_count; k++) {
            uint32_t j = companion_images[k];
            image_barrier(dev, cmd, sc->images[j], written, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, write, 0,
                          writer, bottom);
        }
        image_barrier(dev, cmd, sc->images[i], read, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, read_access, 0,
                      reader, bottom);
        return dev->fns.end_command_buffer(cmd);
    }

    for (uint32_t k = 0; k < companion_count; k++) {
        uint32_t j = companion_images[k];
        image_barrier(dev, cmd, sc->images[j], VK_IMAGE_LAYOUT_UNDEFINED,
                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, top,
                      transfer);
        image_barrier(dev, cmd, sc->history, sc->history_layout,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                      VK_ACCESS_TRANSFER_READ_BIT, transfer, transfer);
        sc->history_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        copy_whole(dev, cmd, sc->history, sc->images[j], sc->extent);
        image_barrier(dev, cmd, sc->images[j], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0, transfer,
                      bottom);
    }

    image_barrier(dev, cmd, sc->images[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, VK_ACCESS_TRANSFER_READ_BIT, top,
                  transfer);
    VkAccessFlags history_access =
        sc->history_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_TRANSFER_READ_BIT;
    image_barrier(dev, cmd, sc->history, sc->history_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  history_access, VK_ACCESS_TRANSFER_WRITE_BIT, transfer, transfer);
    sc->history_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    copy_whole(dev, cmd, sc->images[i], sc->history, sc->extent);
    image_barrier(dev, cmd, sc->images[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT, 0, transfer,
                  bottom);

    return dev->fns.end_command_buffer(cmd);
}
