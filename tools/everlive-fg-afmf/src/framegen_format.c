/* Frame generation: what a swapchain format means for the colour ring, the interpolator and the
 * two direct paths. */

#include "framegen_internal.h"

/* Swapchain format -> how the colour ring is viewed, what the interpolator writes, and how the
 * luma pass should read the values. False for formats without a variant. */
bool afmf_framegen_describe_format(VkFormat swapchain_format, struct afmf_framegen *fg)
{
    switch (swapchain_format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        fg->color_format = VK_FORMAT_R8G8B8A8_UNORM;
        fg->out_format = VK_FORMAT_R8G8B8A8_UNORM;
        fg->variant = VARIANT_RGBA8;
        break;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        fg->color_format = VK_FORMAT_B8G8R8A8_UNORM;
        fg->out_format = VK_FORMAT_R8G8B8A8_UNORM; /* the shader swaps channels for the raw copy */
        fg->variant = VARIANT_RGBA8_BGRA;
        break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        fg->color_format = swapchain_format;
        fg->out_format = swapchain_format;
        fg->variant = VARIANT_RGB10A2;
        break;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        fg->color_format = swapchain_format;
        fg->out_format = swapchain_format;
        fg->variant = VARIANT_RGBA16F;
        break;
    default:
        return false;
    }
    /* Encoded 8/10-bit values are fed as-is (transfer function 0: plain Rec.709 luma), which is
     * perceptually uniform enough for block matching. scRGB half floats go through the SDK's
     * scRGB path with the 80 nit reference white. */
    bool scrgb = fg->variant == VARIANT_RGBA16F;
    fg->transfer_function = scrgb ? 2u : 0u;
    fg->min_luminance = 0.0f;
    fg->max_luminance = scrgb ? 80.0f : 1.0f;
    return true;
}

bool afmf_framegen_format_supports(const struct afmf_device *dev, VkFormat format,
                                   VkFormatFeatureFlags features)
{
    if (dev->ifns.get_format_properties == NULL)
        return false;
    VkFormatProperties props;
    dev->ifns.get_format_properties(dev->physical_device, format, &props);
    return (props.optimalTilingFeatures & features) == features;
}

/* The interpolate variant that stores into a view of the swapchain's own format, or VARIANT_COUNT
 * when none can (sRGB formats take no storage writes; B8G8R8A8 has no SPIR-V format). */
enum variant afmf_framegen_direct_variant_for(const struct afmf_device *dev, VkFormat swapchain_format)
{
    if (!afmf_framegen_format_supports(dev, swapchain_format, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        return VARIANT_COUNT;
    switch (swapchain_format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
        return VARIANT_RGBA8;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        return VARIANT_RGB10A2;
    case VK_FORMAT_R16G16B16A16_SFLOAT:
        return VARIANT_RGBA16F;
    case VK_FORMAT_B8G8R8A8_UNORM:
        return dev->storage_write_without_format ? VARIANT_NOFORMAT : VARIANT_COUNT;
    default:
        return VARIANT_COUNT;
    }
}

bool afmf_framegen_can_write_direct(const struct afmf_device *dev, VkFormat swapchain_format)
{
    return afmf_framegen_direct_variant_for(dev, swapchain_format) != VARIANT_COUNT;
}

bool afmf_framegen_direct(const struct afmf_framegen *fg)
{
    return fg->direct;
}

/* The ingest variant that reads a swapchain image of this format as is and stores the ring, or
 * VARIANT_COUNT: sRGB formats decode on sampling (the ring keeps encoded bytes), and the ring
 * must take storage writes. */
enum variant afmf_framegen_ingest_variant_for(const struct afmf_device *dev, VkFormat swapchain_format)
{
    struct afmf_framegen probe = {0};
    if (!afmf_framegen_describe_format(swapchain_format, &probe) || probe.color_format != swapchain_format)
        return VARIANT_COUNT;
    if (!afmf_framegen_format_supports(dev, swapchain_format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) ||
        !afmf_framegen_format_supports(dev, probe.color_format, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        return VARIANT_COUNT;
    if (probe.variant == VARIANT_RGBA8_BGRA && !dev->storage_write_without_format)
        return VARIANT_COUNT;
    return probe.variant;
}

bool afmf_framegen_can_ingest_direct(const struct afmf_device *dev, VkFormat swapchain_format)
{
    return afmf_framegen_ingest_variant_for(dev, swapchain_format) != VARIANT_COUNT;
}

bool afmf_framegen_ingest_direct(const struct afmf_framegen *fg)
{
    return fg->ingest_direct;
}
