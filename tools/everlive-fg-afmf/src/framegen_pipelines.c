/* Frame generation: the per-device pipelines, compiled on a thread of their own from
 * vkCreateDevice so the first swapchain only waits for what is left. */

#include "framegen_internal.h"

static VkResult create_compute_pipeline(struct afmf_device *dev, const uint32_t *spirv,
                                        size_t spirv_size, VkPipelineLayout layout,
                                        const VkSpecializationInfo *specialization, VkPipeline *out)
{
    VkShaderModuleCreateInfo module_info = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = spirv_size,
        .pCode = spirv,
    };
    VkShaderModule module;
    VkResult res = dev->fns.create_shader_module(dev->handle, &module_info, NULL, &module);
    if (res != VK_SUCCESS)
        return res;

    VkComputePipelineCreateInfo pipeline_info = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
            .pSpecializationInfo = specialization,
        },
        .layout = layout,
    };
    res = dev->fns.create_compute_pipelines(dev->handle, VK_NULL_HANDLE, 1, &pipeline_info, NULL, out);
    dev->fns.destroy_shader_module(dev->handle, module, NULL);
    return res;
}

static VkResult pipelines_create(struct afmf_device *dev);

/* Waits for the background compilation, if one was started, so dev->framegen_pipelines and
 * pipelines_result are settled for the caller. */
static void pipelines_join(struct afmf_device *dev)
{
    if (!dev->pipelines_thread_running)
        return;
    (void)pthread_join(dev->pipelines_thread, NULL);
    dev->pipelines_thread_running = false;
}

static void *pipelines_thread_main(void *arg)
{
    struct afmf_device *dev = arg;
    dev->pipelines_result = pipelines_create(dev);
    return NULL;
}

void afmf_framegen_pipelines_prepare(struct afmf_device *dev)
{
    if (dev->framegen_pipelines != NULL || dev->pipelines_thread_running)
        return;
    dev->pipelines_thread_running =
        pthread_create(&dev->pipelines_thread, NULL, pipelines_thread_main, dev) == 0;
}

void afmf_framegen_pipelines_destroy(struct afmf_device *dev)
{
    pipelines_join(dev);
    struct afmf_framegen_pipelines *p = dev->framegen_pipelines;
    if (p == NULL)
        return;
    for (uint32_t v = 0; v < VARIANT_COUNT; v++)
        if (p->interpolate[v] != VK_NULL_HANDLE)
            dev->fns.destroy_pipeline(dev->handle, p->interpolate[v], NULL);
    for (uint32_t v = 0; v < HALF_COUNT; v++)
        if (p->downsample[v] != VK_NULL_HANDLE)
            dev->fns.destroy_pipeline(dev->handle, p->downsample[v], NULL);
    for (uint32_t v = 0; v < VARIANT_COUNT; v++)
        if (p->ingest[v] != VK_NULL_HANDLE)
            dev->fns.destroy_pipeline(dev->handle, p->ingest[v], NULL);
    for (uint32_t i = 0; i < PASS_COUNT; i++) {
        if (p->pipelines[i] != VK_NULL_HANDLE)
            dev->fns.destroy_pipeline(dev->handle, p->pipelines[i], NULL);
        if (p->layouts[i] != VK_NULL_HANDLE)
            dev->fns.destroy_pipeline_layout(dev->handle, p->layouts[i], NULL);
        if (p->set_layouts[i] != VK_NULL_HANDLE)
            dev->fns.destroy_descriptor_set_layout(dev->handle, p->set_layouts[i], NULL);
    }
    free(p);
    dev->framegen_pipelines = NULL;
}

static VkResult pipelines_create(struct afmf_device *dev)
{
    struct afmf_framegen_pipelines *p = calloc(1, sizeof *p);
    if (p == NULL)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    dev->framegen_pipelines = p;

    for (uint32_t i = 0; i < PASS_COUNT; i++) {
        const struct pass_desc *pass = &passes[i];
        VkDescriptorSetLayoutBinding bindings[16];
        for (uint32_t b = 0; b < pass->binding_count; b++) {
            bindings[b] = (VkDescriptorSetLayoutBinding){
                .binding = b,
                .descriptorType = pass->bindings[b],
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            };
        }
        VkDescriptorSetLayoutCreateInfo set_info = {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = pass->binding_count,
            .pBindings = bindings,
        };
        VkResult res = dev->fns.create_descriptor_set_layout(dev->handle, &set_info, NULL,
                                                             &p->set_layouts[i]);
        if (res != VK_SUCCESS)
            return res;

        VkPushConstantRange push = {
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .size = pass->push_size,
        };
        VkPipelineLayoutCreateInfo layout_info = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &p->set_layouts[i],
            .pushConstantRangeCount = pass->push_size != 0 ? 1u : 0u,
            .pPushConstantRanges = &push,
        };
        res = dev->fns.create_pipeline_layout(dev->handle, &layout_info, NULL, &p->layouts[i]);
        if (res != VK_SUCCESS)
            return res;

        if (i == PASS_INTERPOLATE || i == PASS_DOWNSAMPLE || i == PASS_INGEST)
            continue;
        /* The search's static-block threshold (afmfStaticBlockSad in the patched v5 header). */
        uint32_t static_sad = afmf_config_get()->static_block_sad;
        VkSpecializationMapEntry entry = {.constantID = 0, .offset = 0, .size = sizeof static_sad};
        VkSpecializationInfo search_constants = {
            .mapEntryCount = 1,
            .pMapEntries = &entry,
            .dataSize = sizeof static_sad,
            .pData = &static_sad,
        };
        const uint32_t *spirv = pass->spirv;
        size_t spirv_size = pass->spirv_size;
        if (i == PASS_SEARCH && dev->shader_int16) {
            spirv = ffx_opticalflow_compute_optical_flow_advanced_pass_v5_int16_spv;
            spirv_size = ffx_opticalflow_compute_optical_flow_advanced_pass_v5_int16_spv_size;
        }
        res = create_compute_pipeline(dev, spirv, spirv_size, p->layouts[i],
                                      i == PASS_SEARCH ? &search_constants : NULL, &p->pipelines[i]);
        if (res != VK_SUCCESS)
            return res;
    }
    for (uint32_t v = 0; v < VARIANT_COUNT; v++) {
        if (v == VARIANT_NOFORMAT && !dev->storage_write_without_format)
            continue; /* needs the feature the application did not enable; never used then */
        VkResult res = create_compute_pipeline(dev, interpolate_variants[v].spirv,
                                               interpolate_variants[v].spirv_size,
                                               p->layouts[PASS_INTERPOLATE], NULL, &p->interpolate[v]);
        if (res != VK_SUCCESS)
            return res;
    }
    for (uint32_t v = 0; v < HALF_COUNT; v++) {
        VkResult res = create_compute_pipeline(dev, downsample_variants[v].spirv,
                                               downsample_variants[v].spirv_size,
                                               p->layouts[PASS_DOWNSAMPLE], NULL, &p->downsample[v]);
        if (res != VK_SUCCESS)
            return res;
    }
    for (uint32_t v = 0; v < VARIANT_COUNT; v++) {
        if ((v == VARIANT_NOFORMAT || v == VARIANT_RGBA8_BGRA) && !dev->storage_write_without_format)
            continue;
        VkResult res = create_compute_pipeline(dev, ingest_variants[v].spirv,
                                               ingest_variants[v].spirv_size, p->layouts[PASS_INGEST],
                                               NULL, &p->ingest[v]);
        if (res != VK_SUCCESS)
            return res;
    }
    return VK_SUCCESS;
}

/* The device's pipelines: compiled at vkCreateDevice on a thread of their own
 * (afmf_framegen_pipelines_prepare), so the first swapchain only waits for what is left. */
struct afmf_framegen_pipelines *afmf_framegen_pipelines_get(struct afmf_device *dev)
{
    pipelines_join(dev);
    if (dev->framegen_pipelines == NULL)
        dev->pipelines_result = pipelines_create(dev);
    if (dev->pipelines_result != VK_SUCCESS) {
        AFMF_WARN("frame generation pipelines failed (VkResult %d)", (int)dev->pipelines_result);
        afmf_framegen_pipelines_destroy(dev);
        return NULL;
    }
    return dev->framegen_pipelines;
}
