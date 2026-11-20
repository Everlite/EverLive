/* The swapchain and the staging buffer that feeds it, apart from the device so that a run can
 * take them down and build them again at another resolution, as a game does on a window change. */

#include "headless.h"

/* Zero for the size asks for the default (or AFMF_TEST_EXTENT). */
bool create_swapchain(struct ctx *ctx, uint32_t width, uint32_t height)
{
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx->physical_device, ctx->surface, &caps));
    uint32_t format_count = 1;
    VkSurfaceFormatKHR format;
    VkResult res = vkGetPhysicalDeviceSurfaceFormatsKHR(ctx->physical_device, ctx->surface,
                                                        &format_count, &format);
    if ((res != VK_SUCCESS && res != VK_INCOMPLETE) || format_count == 0)
        return false;

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) { /* the surface lets the swapchain choose */
        extent.width = 640;
        extent.height = 480;
        /* AFMF_TEST_EXTENT=WxH to time the layer at a game-like resolution. */
        const char *wanted = getenv("AFMF_TEST_EXTENT");
        unsigned w = 0, h = 0;
        if (wanted != NULL && sscanf(wanted, "%ux%u", &w, &h) == 2 && w >= 128 && h >= 128 &&
            w <= 8192 && h <= 8192) {
            extent.width = w;
            extent.height = h;
        }
        if (width >= 128 && height >= 128) { /* a resolution this run asked for */
            extent.width = width;
            extent.height = height;
        }
    }
    uint32_t image_count = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && image_count > caps.maxImageCount)
        image_count = caps.maxImageCount;

    static const VkPresentModeKHR fifo_only[1] = {VK_PRESENT_MODE_FIFO_KHR};
    VkSwapchainPresentModesCreateInfoEXT allowed_modes = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT,
        .presentModeCount = 1,
        .pPresentModes = fifo_only,
    };
    VkSwapchainCreateInfoKHR swapchain = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .pNext = ctx->present_modes ? &allowed_modes : NULL,
        .surface = ctx->surface,
        .minImageCount = image_count,
        .imageFormat = format.format,
        .imageColorSpace = format.colorSpace,
        .imageExtent = extent,
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    CHECK(vkCreateSwapchainKHR(ctx->device, &swapchain, NULL, &ctx->swapchain));

    /* AFMF_TEST_EXTRA_IMAGES=<n>: the layer must have added at least n images to what was asked
     * (the driver may round up on its own, so only the lower bound is checked). */
    uint32_t got = 0;
    CHECK(vkGetSwapchainImagesKHR(ctx->device, ctx->swapchain, &got, NULL));
    const char *extra = getenv("AFMF_TEST_EXTRA_IMAGES");
    long expected = extra != NULL ? strtol(extra, NULL, 10) : 0;
    (void)fprintf(stderr, "swapchain has %u images (asked %u)\n", got, image_count);
    if (expected > 0 && got < image_count + (uint32_t)expected) {
        (void)fprintf(stderr, "expected at least %u images\n", image_count + (uint32_t)expected);
        return false;
    }

    /* Host-visible staging for the synthetic frames (RGBA8 or BGRA8: 4 bytes per pixel). */
    ctx->extent = extent;
    VkDeviceSize size = (VkDeviceSize)extent.width * extent.height * 4u;
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    CHECK(vkCreateBuffer(ctx->device, &buffer, NULL, &ctx->staging));
    VkMemoryRequirements reqs;
    vkGetBufferMemoryRequirements(ctx->device, ctx->staging, &reqs);
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(ctx->physical_device, &memory);
    const VkMemoryPropertyFlags wanted =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < memory.memoryTypeCount && type == UINT32_MAX; i++)
        if ((reqs.memoryTypeBits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & wanted) == wanted)
            type = i;
    if (type == UINT32_MAX)
        return false;
    VkMemoryAllocateInfo alloc_memory = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type,
    };
    CHECK(vkAllocateMemory(ctx->device, &alloc_memory, NULL, &ctx->staging_memory));
    CHECK(vkBindBufferMemory(ctx->device, ctx->staging, ctx->staging_memory, 0));
    void *mapped = NULL;
    CHECK(vkMapMemory(ctx->device, ctx->staging_memory, 0, size, 0, &mapped));
    ctx->staging_mapped = mapped;
    return true;
}
