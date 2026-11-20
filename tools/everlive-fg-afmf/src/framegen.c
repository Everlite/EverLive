/* Frame generation: FidelityFX Optical Flow 1.1.2 (seven compute passes, host sequencing ported
 * from the SDK's ffx_opticalflow.cpp, see shaders/fidelityfx/NOTICE.md) plus the layer's own
 * interpolation shader (shaders/afmf_interpolate.comp). This file creates and destroys one
 * swapchain's generation; the pieces live in the framegen_*.c files next to it. */

#include "framegen_internal.h"

void afmf_framegen_set_family(struct afmf_device *dev, struct afmf_framegen *fg, uint32_t family)
{
    if (fg->secondary_pool != VK_NULL_HANDLE || dev->set_loader_data == NULL)
        return;
    uint32_t count = fg->slots * 4u;
    VkCommandPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family,
    };
    if (dev->fns.create_command_pool(dev->handle, &pool, NULL, &fg->secondary_pool) != VK_SUCCESS)
        return;
    VkCommandBuffer *cmds = calloc(count, sizeof *cmds);
    fg->secondaries = calloc(count, sizeof *fg->secondaries);
    VkCommandBufferAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = fg->secondary_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_SECONDARY,
        .commandBufferCount = count,
    };
    bool ok = cmds != NULL && fg->secondaries != NULL &&
              dev->fns.allocate_command_buffers(dev->handle, &alloc, cmds) == VK_SUCCESS;
    /* Allocated below the loader's trampoline: stamp the dispatch pointer ourselves. */
    for (uint32_t i = 0; ok && i < count; i++)
        ok = dev->set_loader_data(dev->handle, cmds[i]) == VK_SUCCESS;
    if (ok) {
        for (uint32_t i = 0; i < count; i++)
            fg->secondaries[i].cmd = cmds[i];
    } else {
        AFMF_WARN("pre-recorded command buffers unavailable; recording every frame");
        dev->fns.destroy_command_pool(dev->handle, fg->secondary_pool, NULL);
        fg->secondary_pool = VK_NULL_HANDLE;
        free(fg->secondaries);
        fg->secondaries = NULL;
    }
    free(cmds);
}

void afmf_framegen_destroy(struct afmf_device *dev, struct afmf_framegen *fg)
{
    if (fg == NULL)
        return;
    if (fg->secondary_pool != VK_NULL_HANDLE)
        dev->fns.destroy_command_pool(dev->handle, fg->secondary_pool, NULL); /* frees the buffers */
    free(fg->secondaries);
    for (uint32_t j = 0; fg->target_views != NULL && j < fg->target_count; j++)
        if (fg->target_views[j] != VK_NULL_HANDLE)
            dev->fns.destroy_image_view(dev->handle, fg->target_views[j], NULL);
    free(fg->target_views); /* source_views is the same array */
    free(fg->direct_sets);
    free(fg->ingest_sets);
    if (fg->pool != VK_NULL_HANDLE)
        dev->fns.destroy_descriptor_pool(dev->handle, fg->pool, NULL); /* frees the sets */
    if (fg->queries != VK_NULL_HANDLE) {
        /* The caller has waited for every slot; fold the last results in and report. */
        for (uint32_t s = 0; s < fg->slots; s++)
            afmf_framegen_profiler_collect(dev, fg, s);
        afmf_framegen_profiler_report(fg);
        dev->fns.destroy_query_pool(dev->handle, fg->queries, NULL);
    }
    free(fg->query_count);
    free(fg->query_stage);
    if (fg->dump_mapped != NULL)
        dev->fns.unmap_memory(dev->handle, fg->dump_memory);
    if (fg->dump_buffer != VK_NULL_HANDLE)
        dev->fns.destroy_buffer(dev->handle, fg->dump_buffer, NULL);
    if (fg->dump_memory != VK_NULL_HANDLE)
        dev->fns.free_memory(dev->handle, fg->dump_memory, NULL);
    if (fg->cb_mapped != NULL)
        dev->fns.unmap_memory(dev->handle, fg->cb_memory);
    if (fg->cb != VK_NULL_HANDLE)
        dev->fns.destroy_buffer(dev->handle, fg->cb, NULL);
    if (fg->cb_memory != VK_NULL_HANDLE)
        dev->fns.free_memory(dev->handle, fg->cb_memory, NULL);
    if (fg->sampler != VK_NULL_HANDLE)
        dev->fns.destroy_sampler(dev->handle, fg->sampler, NULL);
    afmf_framegen_image_destroy(dev, &fg->output);
    afmf_framegen_image_destroy(dev, &fg->color_half);
    for (uint32_t p = 0; p < 2; p++) {
        afmf_framegen_image_destroy(dev, &fg->color[p]);
        for (uint32_t k = 0; k < AFMF_LEVELS; k++) {
            afmf_framegen_image_destroy(dev, &fg->luma[p][k]);
            afmf_framegen_image_destroy(dev, &fg->flow[p][k]);
        }
    }
    afmf_framegen_image_destroy(dev, &fg->flow_out);
    afmf_framegen_image_destroy(dev, &fg->scd_histogram);
    afmf_framegen_image_destroy(dev, &fg->scd_previous_histogram);
    afmf_framegen_image_destroy(dev, &fg->scd_temp);
    afmf_framegen_image_destroy(dev, &fg->scd_output);
    free(fg);
}

/* Why generation cannot run on this device and swapchain, or NULL. */
static const char *blocker_for(struct afmf_device *dev, struct afmf_framegen *fg,
                               VkFormat swapchain_format, VkExtent2D extent, bool half)
{
    /* What the flow shaders use: subgroupElect and the ids (basic), subgroupAdd and
     * subgroupMin (arithmetic), the pyramid's quad swaps (quad); all in compute. */
    const VkSubgroupFeatureFlags subgroup_ops = VK_SUBGROUP_FEATURE_BASIC_BIT |
                                                VK_SUBGROUP_FEATURE_ARITHMETIC_BIT |
                                                VK_SUBGROUP_FEATURE_QUAD_BIT;
    if (dev->api_version < VK_API_VERSION_1_1)
        return "application uses Vulkan 1.0; the flow shaders need 1.1 subgroups";
    if ((dev->subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) == 0 ||
        (dev->subgroup.supportedOperations & subgroup_ops) != subgroup_ops)
        return "device lacks subgroup basic, arithmetic and quad operations in compute";
    if (!afmf_framegen_describe_format(swapchain_format, fg))
        return "swapchain format has no interpolation variant";
    if (extent.width < AFMF_MIN_EXTENT || extent.height < AFMF_MIN_EXTENT)
        return "swapchain too small for the optical flow pyramid";
    if (!afmf_framegen_format_supports(dev, fg->out_format, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !afmf_framegen_format_supports(dev, fg->color_format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT))
        return "device lacks storage or filtered sampling for the swapchain format";
    if (half && !fg->ingest_direct &&
        !afmf_framegen_format_supports(dev, downsample_variants[fg->half].format,
                                       VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        return "device lacks storage for the half-resolution colour";
    if (!afmf_framegen_format_supports(dev, VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT))
        return "device lacks r32ui image atomics";
    if (!afmf_framegen_format_supports(dev, VK_FORMAT_R8_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ||
        !afmf_framegen_format_supports(dev, VK_FORMAT_R16G16_SINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT))
        return "device lacks r8ui or rg16i storage images (shaderStorageImageExtendedFormats)";
    if (afmf_framegen_pipelines_get(dev) == NULL)
        return "pipelines unavailable";
    return NULL;
}

struct afmf_framegen *afmf_framegen_create(struct afmf_device *dev, VkFormat swapchain_format,
                                           VkExtent2D extent, uint32_t slots,
                                           const VkImage *images, uint32_t image_count,
                                           bool direct_output, bool direct_ingest)
{
    struct afmf_framegen *fg = calloc(1, sizeof *fg);
    if (fg == NULL)
        return NULL;
    fg->extent = extent;
    fg->slots = slots;
    fg->swapchain_format = swapchain_format;
    fg->dump_from = AFMF_DUMP_FIRST;
    bool have_images = images != NULL && image_count > 0 && image_count <= AFMF_MAX_IMAGES;
    fg->direct_variant = afmf_framegen_direct_variant_for(dev, swapchain_format);
    fg->direct = have_images && direct_output && fg->direct_variant != VARIANT_COUNT;
    fg->ingest_variant = afmf_framegen_ingest_variant_for(dev, swapchain_format);
    fg->ingest_direct = have_images && direct_ingest && fg->ingest_variant != VARIANT_COUNT;
    fg->target_count = fg->direct || fg->ingest_direct ? image_count : 0;

    /* Performance mode: the block search, 85 % of the cost, runs on a half-size frame; blocks
     * become 16 pixels on screen. Auto picks it from 1440p up, where the search dominates. */
    const struct afmf_config *cfg = afmf_config_get();
    bool half = cfg->performance == AFMF_PERFORMANCE_FAST ||
                (cfg->performance == AFMF_PERFORMANCE_AUTO &&
                 (uint64_t)extent.width * extent.height >= 2560u * 1440u);
    if (extent.width / 2 < AFMF_MIN_EXTENT || extent.height / 2 < AFMF_MIN_EXTENT)
        half = false; /* too small for the pyramid at half size: full resolution instead */
    fg->flow_scale = half ? 2u : 1u;
    fg->of_extent.width = (extent.width + fg->flow_scale - 1) / fg->flow_scale;
    fg->of_extent.height = (extent.height + fg->flow_scale - 1) / fg->flow_scale;
    fg->half = swapchain_format == VK_FORMAT_R16G16B16A16_SFLOAT ? HALF_RGBA16F : HALF_RGBA8;
    /* At reduced flow resolution five levels already cover +-128 flow pixels (+-256 on screen): the
     * two coarsest levels are drains, not accuracy. `high` still forces all seven. */
    fg->levels = cfg->search_mode == AFMF_SEARCH_STANDARD ? 5u : 7u;
    if (fg->flow_scale > 1 && cfg->search_mode != AFMF_SEARCH_HIGH && fg->levels > 5)
        fg->levels = 5;
    if (fg->levels > AFMF_LEVELS)
        fg->levels = AFMF_LEVELS;
    fg->max_levels = fg->levels;

    const char *blocker = blocker_for(dev, fg, swapchain_format, extent, half);
    VkResult res = VK_SUCCESS;
    if (blocker == NULL && (fg->direct || fg->ingest_direct)) {
        /* One view per swapchain image, in the swapchain's own format: storage for the direct
         * output, sampled for the direct ingest (the same view serves both). */
        fg->target_views = calloc(fg->target_count, sizeof *fg->target_views);
        if (fg->target_views == NULL)
            res = VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t j = 0; res == VK_SUCCESS && j < fg->target_count; j++) {
            VkImageViewCreateInfo view = {
                .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                .image = images[j],
                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                .format = swapchain_format,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            };
            res = dev->fns.create_image_view(dev->handle, &view, NULL, &fg->target_views[j]);
        }
        fg->source_views = fg->target_views;
        if (res != VK_SUCCESS)
            blocker = "views of the swapchain images failed";
    }
    if (blocker == NULL) {
        res = afmf_framegen_resources_create(dev, fg);
        if (res == VK_SUCCESS)
            res = afmf_framegen_descriptor_sets_create(dev, fg);
        if (res != VK_SUCCESS)
            blocker = "resource creation failed";
        else {
            afmf_framegen_dump_buffer_create(dev, fg);
            afmf_framegen_profiler_create(dev, fg);
        }
    }
    if (blocker != NULL) {
        AFMF_WARN("interpolation unavailable for this swapchain: %s (VkResult %d); repeating frames",
                  blocker, (int)res);
        afmf_framegen_destroy(dev, fg);
        return NULL;
    }
    AFMF_INFO("interpolation ready: %ux%u, flow at %ux%u (%ux%u blocks of %u px), %u pyramid "
              "levels, %s SAD%s%s",
              extent.width, extent.height, fg->of_extent.width, fg->of_extent.height,
              fg->flow_size[0].width, fg->flow_size[0].height, AFMF_FLOW_BLOCK * fg->flow_scale,
              fg->levels, dev->shader_int16 ? "packed 16-bit" : "scalar",
              fg->ingest_direct ? ", direct ingest" : "", fg->direct ? ", direct output" : "");
    return fg;
}
