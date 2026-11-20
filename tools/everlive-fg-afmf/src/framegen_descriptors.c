/* Frame generation: the descriptor sets, built once per swapchain for both frame parities so
 * the flow and the interpolation are a fixed sequence of bind + dispatch. */

#include "framegen_internal.h"
#include "framegen_writer.h"

static VkResult sets_allocate(struct afmf_device *dev, struct afmf_framegen *fg,
                              const struct afmf_framegen_pipelines *pl, uint32_t direct_sets,
                              uint32_t ingest_sets)
{
    VkDescriptorSetLayout layouts[SET_COUNT];
    layouts[SET_PREPARE] = pl->set_layouts[PASS_PREPARE_LUMA];
    layouts[SET_PYRAMID] = pl->set_layouts[PASS_PYRAMID];
    layouts[SET_SCD_HISTOGRAM] = pl->set_layouts[PASS_SCD_HISTOGRAM];
    layouts[SET_SCD_DIVERGENCE] = pl->set_layouts[PASS_SCD_DIVERGENCE];
    for (uint32_t k = 0; k < AFMF_LEVELS; k++) {
        layouts[SET_SEARCH + k] = pl->set_layouts[PASS_SEARCH];
        layouts[SET_FILTER + k] = pl->set_layouts[PASS_FILTER];
    }
    for (uint32_t k = 1; k < AFMF_LEVELS; k++)
        layouts[SET_SCALE + k - 1] = pl->set_layouts[PASS_SCALE];
    layouts[SET_INTERPOLATE] = pl->set_layouts[PASS_INTERPOLATE];
    layouts[SET_DOWNSAMPLE] = pl->set_layouts[PASS_DOWNSAMPLE];

    VkResult res;
    for (uint32_t p = 0; p < 2; p++) {
        VkDescriptorSetAllocateInfo alloc = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = fg->pool,
            .descriptorSetCount = SET_COUNT,
            .pSetLayouts = layouts,
        };
        res = dev->fns.allocate_descriptor_sets(dev->handle, &alloc, fg->sets[p]);
        if (res != VK_SUCCESS)
            return res;
    }
    if (fg->direct) {
        VkDescriptorSetLayout interpolate_layouts[AFMF_MAX_IMAGES];
        for (uint32_t j = 0; j < fg->target_count; j++)
            interpolate_layouts[j] = pl->set_layouts[PASS_INTERPOLATE];
        fg->direct_sets = calloc(direct_sets, sizeof *fg->direct_sets);
        if (fg->direct_sets == NULL)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t p = 0; p < 2; p++) {
            VkDescriptorSetAllocateInfo alloc = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = fg->pool,
                .descriptorSetCount = fg->target_count,
                .pSetLayouts = interpolate_layouts,
            };
            res = dev->fns.allocate_descriptor_sets(dev->handle, &alloc,
                                                    fg->direct_sets + p * fg->target_count);
            if (res != VK_SUCCESS)
                return res;
        }
    }
    if (fg->ingest_direct) {
        VkDescriptorSetLayout ingest_layouts[AFMF_MAX_IMAGES];
        for (uint32_t j = 0; j < fg->target_count; j++)
            ingest_layouts[j] = pl->set_layouts[PASS_INGEST];
        fg->ingest_sets = calloc(ingest_sets, sizeof *fg->ingest_sets);
        if (fg->ingest_sets == NULL)
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t p = 0; p < 2; p++) {
            VkDescriptorSetAllocateInfo alloc = {
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = fg->pool,
                .descriptorSetCount = fg->target_count,
                .pSetLayouts = ingest_layouts,
            };
            res = dev->fns.allocate_descriptor_sets(dev->handle, &alloc,
                                                    fg->ingest_sets + p * fg->target_count);
            if (res != VK_SUCCESS)
                return res;
        }
    }
    return VK_SUCCESS;
}

static void sets_write(struct afmf_device *dev, struct afmf_framegen *fg, uint32_t p)
{
    uint32_t q = 1u - p; /* the other parity: previous frame's luma, previous colour */
    struct set_writer w;

    /* Unused with direct ingest, which writes the luma itself; the half-size colour it would
     * read does not exist then. */
    writer_begin(&w, dev, fg->sets[p][SET_PREPARE]);
    writer_image(&w, 0, SAMPLED,
                 fg->flow_scale > 1 && !fg->ingest_direct ? fg->color_half.view : fg->color[p].view,
                 VK_NULL_HANDLE);
    writer_image(&w, 1, STORAGE, fg->luma[p][0].view, VK_NULL_HANDLE);
    writer_ubo(&w, 2, fg->cb);
    writer_end(&w);

    writer_begin(&w, dev, fg->sets[p][SET_PYRAMID]);
    for (uint32_t k = 0; k < AFMF_LEVELS; k++)
        writer_image(&w, k, STORAGE, fg->luma[p][k].view, VK_NULL_HANDLE);
    writer_ubo(&w, 7, fg->cb);
    writer_ubo(&w, 8, fg->cb);
    writer_end(&w);

    writer_begin(&w, dev, fg->sets[p][SET_SCD_HISTOGRAM]);
    writer_image(&w, 0, SAMPLED, fg->luma[p][0].view, VK_NULL_HANDLE); /* see record_flow */
    writer_image(&w, 1, STORAGE, fg->scd_histogram.view, VK_NULL_HANDLE);
    writer_ubo(&w, 2, fg->cb);
    writer_end(&w);

    writer_begin(&w, dev, fg->sets[p][SET_SCD_DIVERGENCE]);
    writer_image(&w, 0, STORAGE, fg->scd_histogram.view, VK_NULL_HANDLE);
    writer_image(&w, 1, STORAGE, fg->scd_previous_histogram.view, VK_NULL_HANDLE);
    writer_image(&w, 2, STORAGE, fg->scd_temp.view, VK_NULL_HANDLE);
    writer_image(&w, 3, STORAGE, fg->scd_output.view, VK_NULL_HANDLE);
    writer_ubo(&w, 4, fg->cb);
    writer_end(&w);

    for (uint32_t k = 0; k < AFMF_LEVELS; k++) {
        uint32_t fa = afmf_flow_parity_a(p, k);
        uint32_t fb = 1u - fa;

        writer_begin(&w, dev, fg->sets[p][SET_SEARCH + k]);
        writer_image(&w, 0, SAMPLED, fg->luma[p][k].view, VK_NULL_HANDLE);
        writer_image(&w, 1, SAMPLED, fg->luma[q][k].view, VK_NULL_HANDLE);
        writer_image(&w, 2, STORAGE, fg->flow[fa][k].view, VK_NULL_HANDLE);
        writer_image(&w, 3, STORAGE, fg->scd_output.view, VK_NULL_HANDLE);
        writer_ubo(&w, 4, fg->cb);
        writer_end(&w);

        writer_begin(&w, dev, fg->sets[p][SET_FILTER + k]);
        writer_image(&w, 0, SAMPLED, fg->flow[fa][k].view, VK_NULL_HANDLE);
        writer_image(&w, 1, STORAGE, k == 0 ? fg->flow_out.view : fg->flow[fb][k].view,
                     VK_NULL_HANDLE);
        writer_ubo(&w, 2, fg->cb);
        writer_end(&w);

        if (k == 0)
            continue;
        writer_begin(&w, dev, fg->sets[p][SET_SCALE + k - 1]);
        writer_image(&w, 0, SAMPLED, fg->luma[p][k].view, VK_NULL_HANDLE);
        writer_image(&w, 1, SAMPLED, fg->luma[q][k].view, VK_NULL_HANDLE);
        writer_image(&w, 2, SAMPLED, fg->flow[fb][k].view, VK_NULL_HANDLE);
        writer_image(&w, 3, STORAGE, fg->flow[fb][k - 1].view, VK_NULL_HANDLE);
        writer_image(&w, 4, STORAGE, fg->scd_output.view, VK_NULL_HANDLE);
        writer_ubo(&w, 5, fg->cb);
        writer_end(&w);
    }

    for (uint32_t j = 0; j < (fg->direct ? fg->target_count : 1u); j++) {
        writer_begin(&w, dev, fg->direct ? fg->direct_sets[p * fg->target_count + j]
                                         : fg->sets[p][SET_INTERPOLATE]);
        writer_image(&w, 0, COMBINED, fg->color[q].view, fg->sampler);
        writer_image(&w, 1, COMBINED, fg->color[p].view, fg->sampler);
        writer_image(&w, 2, SAMPLED, fg->flow_out.view, VK_NULL_HANDLE);
        writer_image(&w, 3, STORAGE, fg->scd_output.view, VK_NULL_HANDLE);
        writer_image(&w, 4, STORAGE, fg->direct ? fg->target_views[j] : fg->output.view,
                     VK_NULL_HANDLE);
        writer_end(&w);
    }

    if (fg->flow_scale > 1 && !fg->ingest_direct) {
        writer_begin(&w, dev, fg->sets[p][SET_DOWNSAMPLE]);
        writer_image(&w, 0, COMBINED, fg->color[p].view, fg->sampler);
        writer_image(&w, 1, STORAGE, fg->color_half.view, VK_NULL_HANDLE);
        writer_end(&w);
    }

    for (uint32_t j = 0; fg->ingest_direct && j < fg->target_count; j++) {
        writer_begin(&w, dev, fg->ingest_sets[p * fg->target_count + j]);
        writer_image(&w, 0, SAMPLED, fg->source_views[j], VK_NULL_HANDLE);
        writer_image(&w, 1, STORAGE, fg->color[p].view, VK_NULL_HANDLE);
        writer_image(&w, 2, STORAGE, fg->luma[p][0].view, VK_NULL_HANDLE);
        writer_end(&w);
    }
}

VkResult afmf_framegen_descriptor_sets_create(struct afmf_device *dev, struct afmf_framegen *fg)
{
    const struct afmf_framegen_pipelines *pl = dev->framegen_pipelines;

    uint32_t direct_sets = fg->direct ? 2u * fg->target_count : 0u; /* one interpolate set each */
    uint32_t ingest_sets = fg->ingest_direct ? 2u * fg->target_count : 0u;
    VkDescriptorPoolSize sizes[] = {
        {SAMPLED, 2 * 42 + direct_sets + ingest_sets},
        {STORAGE, 2 * 49 + 2 * direct_sets + 2 * ingest_sets},
        {COMBINED, 2 * 3 + 2 * direct_sets},
        {UBO, 2 * 25},
    };
    VkDescriptorPoolCreateInfo pool = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 2 * SET_COUNT + direct_sets + ingest_sets,
        .poolSizeCount = (uint32_t)(sizeof sizes / sizeof sizes[0]),
        .pPoolSizes = sizes,
    };
    VkResult res = dev->fns.create_descriptor_pool(dev->handle, &pool, NULL, &fg->pool);
    if (res != VK_SUCCESS)
        return res;
    res = sets_allocate(dev, fg, pl, direct_sets, ingest_sets);
    if (res != VK_SUCCESS)
        return res;
    for (uint32_t p = 0; p < 2; p++)
        sets_write(dev, fg, p);
    return VK_SUCCESS;
}
