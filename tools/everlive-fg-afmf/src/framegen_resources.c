/* Frame generation: the images, the sampler and the constants buffer one swapchain's generation
 * lives on. */

#include "framegen_internal.h"

bool afmf_framegen_find_memory_type(const struct afmf_device *dev, uint32_t type_bits,
                                    VkMemoryPropertyFlags wanted, uint32_t *out)
{
    const VkPhysicalDeviceMemoryProperties *props = &dev->memory_properties;
    for (uint32_t i = 0; i < props->memoryTypeCount; i++) {
        if ((type_bits & (1u << i)) && (props->memoryTypes[i].propertyFlags & wanted) == wanted) {
            *out = i;
            return true;
        }
    }
    return false;
}

static VkResult image_create(struct afmf_device *dev, struct image *img, VkFormat format,
                             VkExtent2D extent, VkImageUsageFlags usage)
{
    VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {extent.width, extent.height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult res = dev->fns.create_image(dev->handle, &info, NULL, &img->image);
    if (res != VK_SUCCESS)
        return res;

    VkMemoryRequirements reqs;
    dev->fns.get_image_memory_requirements(dev->handle, img->image, &reqs);
    uint32_t type;
    if (!afmf_framegen_find_memory_type(dev, reqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, &type) &&
        !afmf_framegen_find_memory_type(dev, reqs.memoryTypeBits, 0, &type))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type,
    };
    res = dev->fns.allocate_memory(dev->handle, &alloc, NULL, &img->memory);
    if (res != VK_SUCCESS)
        return res;
    res = dev->fns.bind_image_memory(dev->handle, img->image, img->memory, 0);
    if (res != VK_SUCCESS)
        return res;

    VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
    return dev->fns.create_image_view(dev->handle, &view, NULL, &img->view);
}

void afmf_framegen_image_destroy(struct afmf_device *dev, struct image *img)
{
    if (img->view != VK_NULL_HANDLE)
        dev->fns.destroy_image_view(dev->handle, img->view, NULL);
    if (img->image != VK_NULL_HANDLE)
        dev->fns.destroy_image(dev->handle, img->image, NULL);
    if (img->memory != VK_NULL_HANDLE)
        dev->fns.free_memory(dev->handle, img->memory, NULL);
    memset(img, 0, sizeof *img);
}

static uint32_t align_up(uint32_t value, uint32_t alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

VkResult afmf_framegen_resources_create(struct afmf_device *dev, struct afmf_framegen *fg)
{
    /* TRANSFER_SRC only serves the AFMF_DUMP_DIR readback of luma and flow. */
    const VkImageUsageFlags internal = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                       VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult res;

    /* Luma pyramid: level k is the flow resolution >> k; flow: 8x8 blocks, then halved. */
    for (uint32_t k = 0; k < AFMF_LEVELS; k++) {
        fg->luma_size[k].width = fg->of_extent.width >> k ? fg->of_extent.width >> k : 1u;
        fg->luma_size[k].height = fg->of_extent.height >> k ? fg->of_extent.height >> k : 1u;
        if (k == 0) {
            fg->flow_size[0].width = (fg->of_extent.width + AFMF_FLOW_BLOCK - 1) / AFMF_FLOW_BLOCK;
            fg->flow_size[0].height = (fg->of_extent.height + AFMF_FLOW_BLOCK - 1) / AFMF_FLOW_BLOCK;
        } else {
            fg->flow_size[k].width = (fg->flow_size[k - 1].width + 1) / 2;
            fg->flow_size[k].height = (fg->flow_size[k - 1].height + 1) / 2;
        }
        for (uint32_t parity = 0; parity < 2; parity++) {
            res = image_create(dev, &fg->luma[parity][k], VK_FORMAT_R8_UINT, fg->luma_size[k],
                               internal);
            if (res != VK_SUCCESS)
                return res;
            res = image_create(dev, &fg->flow[parity][k], VK_FORMAT_R16G16_SINT, fg->flow_size[k],
                               internal);
            if (res != VK_SUCCESS)
                return res;
        }
    }
    res = image_create(dev, &fg->flow_out, VK_FORMAT_R16G16_SINT, fg->flow_size[0], internal);
    if (res != VK_SUCCESS)
        return res;

    VkExtent2D histogram = {AFMF_HISTOGRAM_BINS * AFMF_HISTOGRAMS_PER_DIM * AFMF_HISTOGRAMS_PER_DIM, 1};
    VkExtent2D scd = {AFMF_SCD_SLOTS, 1};
    res = image_create(dev, &fg->scd_histogram, VK_FORMAT_R32_UINT, histogram, internal);
    if (res == VK_SUCCESS)
        res = image_create(dev, &fg->scd_previous_histogram, VK_FORMAT_R32_SFLOAT, histogram, internal);
    if (res == VK_SUCCESS)
        res = image_create(dev, &fg->scd_temp, VK_FORMAT_R32_UINT, scd, internal);
    if (res == VK_SUCCESS)
        res = image_create(dev, &fg->scd_output, VK_FORMAT_R32_UINT, scd, internal);
    if (res != VK_SUCCESS)
        return res;

    const VkImageUsageFlags colour = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                     (fg->ingest_direct ? VK_IMAGE_USAGE_STORAGE_BIT : 0u);
    for (uint32_t parity = 0; parity < 2; parity++) {
        res = image_create(dev, &fg->color[parity], fg->color_format, fg->extent, colour);
        if (res != VK_SUCCESS)
            return res;
    }
    /* Only the copy path needs it: direct ingest writes the luma at the flow's size itself. */
    if (fg->flow_scale > 1 && !fg->ingest_direct) {
        res = image_create(dev, &fg->color_half, downsample_variants[fg->half].format, fg->of_extent,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT);
        if (res != VK_SUCCESS)
            return res;
    }
    if (!fg->direct) {
        res = image_create(dev, &fg->output, fg->out_format, fg->extent,
                           VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        if (res != VK_SUCCESS)
            return res;
    }

    VkSamplerCreateInfo sampler = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    res = dev->fns.create_sampler(dev->handle, &sampler, NULL, &fg->sampler);
    if (res != VK_SUCCESS)
        return res;

    /* Constants: one host-coherent buffer, [slot][level 0..6, spd] regions at the UBO alignment. */
    uint32_t alignment = (uint32_t)dev->limits.minUniformBufferOffsetAlignment;
    fg->cb_stride = align_up(AFMF_CB_SIZE, alignment != 0 ? alignment : 1u);
    VkDeviceSize cb_size = (VkDeviceSize)fg->cb_stride * AFMF_CB_PER_SLOT * fg->slots;
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = cb_size,
        .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    res = dev->fns.create_buffer(dev->handle, &buffer, NULL, &fg->cb);
    if (res != VK_SUCCESS)
        return res;
    VkMemoryRequirements reqs;
    dev->fns.get_buffer_memory_requirements(dev->handle, fg->cb, &reqs);
    uint32_t type;
    if (!afmf_framegen_find_memory_type(dev, reqs.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                        &type))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type,
    };
    res = dev->fns.allocate_memory(dev->handle, &alloc, NULL, &fg->cb_memory);
    if (res == VK_SUCCESS)
        res = dev->fns.bind_buffer_memory(dev->handle, fg->cb, fg->cb_memory, 0);
    if (res == VK_SUCCESS) {
        void *mapped = NULL;
        res = dev->fns.map_memory(dev->handle, fg->cb_memory, 0, cb_size, 0, &mapped);
        fg->cb_mapped = mapped;
    }
    return res;
}
