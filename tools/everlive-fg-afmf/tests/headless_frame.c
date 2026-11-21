/* One frame: the synthetic picture painted into the staging buffer, uploaded, presented, and the
 * queue drained so the same two semaphores serve the next one. Correctness, not throughput. */

#include "headless.h"

uint32_t square_x(uint32_t frame)
{
    return SQUARE_X0 + (frame * SQUARE_STEP) % SQUARE_WRAP;
}

static void paint_scroll(struct ctx *ctx, uint32_t frame, bool fog)
{
    uint32_t w = ctx->extent.width, h = ctx->extent.height;
    uint32_t shift = frame * SQUARE_STEP; /* the picture scrolls to the left SQUARE_STEP px per frame */
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = ctx->staging_mapped + (size_t)y * w * 4u;
        for (uint32_t x = 0; x < w; x++) {
            uint32_t sx = x + shift;
            uint32_t hash = (sx * 2654435761u) ^ (y * 2246822519u);
            uint8_t v;
            if (fog) {
                uint32_t base = 120u + (((sx + y) >> 9) & 3u); /* a gradient of four levels */
                v = (uint8_t)(base + (hash >> 30));           /* under noise of 0..3 */
            } else {
                v = (uint8_t)(((hash >> 24) >> 2) + 32u); /* 32..95: a soft texture, not white noise */
            }
            row[x * 4] = v;
            row[x * 4 + 1] = v;
            row[x * 4 + 2] = v;
            row[x * 4 + 3] = 0xff;
        }
    }
}

static void paint_frame(struct ctx *ctx, uint32_t frame)
{
    uint32_t w = ctx->extent.width, h = ctx->extent.height;
    if (wanted_fog() || wanted_pan()) {
        paint_scroll(ctx, frame, wanted_fog());
        return;
    }
    memset(ctx->staging_mapped, 0, (size_t)w * h * 4u);
    uint32_t x0 = square_x(frame);
    /* Two bright levels in a pattern that travels with the square (period 5, which 8 px of
     * motion never lines up with), so the block search sees it move; a flat interior looks the
     * same in both frames and the blocks come out static. Both levels count as bright. */
    for (uint32_t y = SQUARE_Y; y < SQUARE_Y + SQUARE_SIZE && y < h; y++)
        for (uint32_t x = x0; x < x0 + SQUARE_SIZE && x < w; x++)
            memset(ctx->staging_mapped + ((size_t)y * w + x) * 4u,
                   ((x - x0) * 7u + y * 13u) % 5u < 2u ? 0xa0 : 0xff, 4);
    if (wanted_hud_bar()) {
        /* Red in RGBA byte order (a BGRA surface shows it blue; the check accepts either). */
        static const uint8_t red[4] = {0xff, 0x00, 0x00, 0xff};
        for (uint32_t y = BAR_Y0; y < BAR_Y1 && y < h; y++)
            for (uint32_t x = BAR_X; x < BAR_X + BAR_W && x < w; x++)
                memcpy(ctx->staging_mapped + ((size_t)y * w + x) * 4u, red, 4);
    }
}

/* AFMF_TEST_FRAME_MS=<n>: pace the presents like a game at that frame time, so the layer's
 * pacing hold (half of it) shows in its profile line. AFMF_TEST_HITCH=<n>: one present in n takes
 * five frame times instead of one, a stutter of the game's (not a loading pause, which the
 * layer's cadence ignores past 250 ms). What it exercises is the pacing after a hitch: the hold
 * must not run away, no present may lose its companion over it, and the governor must sit at
 * step 0 once the hitches pass. */
static void pace(uint32_t frame)
{
    static long frame_ms = -1, hitch = -1;
    if (frame_ms < 0) {
        const char *wanted = getenv("AFMF_TEST_FRAME_MS");
        frame_ms = wanted != NULL ? strtol(wanted, NULL, 10) : 0;
        if (frame_ms < 0 || frame_ms > 1000)
            frame_ms = 0;
        const char *every = getenv("AFMF_TEST_HITCH");
        hitch = every != NULL ? strtol(every, NULL, 10) : 0;
        if (hitch < 0 || hitch > 100000)
            hitch = 0;
    }
    if (frame_ms <= 0)
        return;
    long ms = hitch > 0 && frame > 0 && frame % (uint32_t)hitch == 0 ? frame_ms * 5 : frame_ms;
    struct timespec pause = {.tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L};
    (void)nanosleep(&pause, NULL);
}

bool present_frame(struct ctx *ctx, uint32_t frame)
{
    paint_frame(ctx, frame);
    uint32_t index = 0;
    CHECK(vkAcquireNextImageKHR(ctx->device, ctx->swapchain, UINT64_MAX, ctx->acquired,
                                VK_NULL_HANDLE, &index));

    uint32_t image_count = 0;
    CHECK(vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &image_count, NULL));
    VkImage *images = calloc(image_count, sizeof *images);
    if (images == NULL)
        return false;
    VkResult res = vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &image_count, images);
    VkImage image = res == VK_SUCCESS && index < image_count ? images[index] : VK_NULL_HANDLE;
    free(images);
    if (image == VK_NULL_HANDLE)
        return false;

    VkCommandBufferBeginInfo begin = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(vkBeginCommandBuffer(ctx->command_buffer, &begin));
    VkImageMemoryBarrier to_transfer = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    vkCmdPipelineBarrier(ctx->command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &to_transfer);
    VkBufferImageCopy region = {
        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .imageExtent = {ctx->extent.width, ctx->extent.height, 1},
    };
    vkCmdCopyBufferToImage(ctx->command_buffer, ctx->staging, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    VkImageMemoryBarrier to_present = to_transfer;
    to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_present.dstAccessMask = 0;
    to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vkCmdPipelineBarrier(ctx->command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &to_present);
    CHECK(vkEndCommandBuffer(ctx->command_buffer));

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkSubmitInfo submit = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &ctx->acquired,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &ctx->command_buffer,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &ctx->ready,
    };
    CHECK(vkQueueSubmit(ctx->queue, 1, &submit, VK_NULL_HANDLE));

    uint64_t present_id = frame + 1;
    VkPresentIdKHR id = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR, .swapchainCount = 1, .pPresentIds = &present_id};
#ifdef VK_KHR_present_id2
    VkPresentId2KHR id2 = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_ID_2_KHR, .swapchainCount = 1, .pPresentIds = &present_id};
#endif
    VkPresentInfoKHR present = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &ctx->ready,
        .swapchainCount = 1,
        .pSwapchains = &ctx->swapchain,
        .pImageIndices = &index,
    };
    if (ctx->present_id == 1)
        present.pNext = &id;
#ifdef VK_KHR_present_id2
    if (ctx->present_id == 2)
        present.pNext = &id2;
#endif
    static const VkPresentModeKHR fifo = VK_PRESENT_MODE_FIFO_KHR;
    VkSwapchainPresentModeInfoEXT mode = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODE_INFO_EXT,
        .pNext = present.pNext,
        .swapchainCount = 1,
        .pPresentModes = &fifo,
    };
    if (ctx->present_modes)
        present.pNext = &mode;
    res = vkQueuePresentKHR(ctx->queue, &present);
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR) {
        (void)fprintf(stderr, "vkQueuePresentKHR -> VkResult %d\n", (int)res);
        return false;
    }
    CHECK(vkQueueWaitIdle(ctx->queue));
    pace(frame);
    return true;
}
