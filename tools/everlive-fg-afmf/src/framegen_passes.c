/* Frame generation: the tables the pipelines are built from. Binding numbers come from the
 * FidelityFX pass files under shaders/fidelityfx/passes/. */

#include "framegen_internal.h"

const char *const stage_names[STAGE_COUNT] = {
    "ingest", "prepare luma", "pyramid + scd histogram", "search (all levels)",
    "filter (all levels)", "scale (all levels)", "interpolate", "output copy",
};

static const VkDescriptorType bindings_prepare[] = {SAMPLED, STORAGE, UBO};
static const VkDescriptorType bindings_pyramid[] = {STORAGE, STORAGE, STORAGE, STORAGE, STORAGE,
                                                    STORAGE, STORAGE, UBO, UBO};
static const VkDescriptorType bindings_scd_histogram[] = {SAMPLED, STORAGE, UBO};
static const VkDescriptorType bindings_scd_divergence[] = {STORAGE, STORAGE, STORAGE, STORAGE, UBO};
static const VkDescriptorType bindings_search[] = {SAMPLED, SAMPLED, STORAGE, STORAGE, UBO};
static const VkDescriptorType bindings_filter[] = {SAMPLED, STORAGE, UBO};
static const VkDescriptorType bindings_scale[] = {SAMPLED, SAMPLED, SAMPLED, STORAGE, STORAGE, UBO};
static const VkDescriptorType bindings_interpolate[] = {COMBINED, COMBINED, SAMPLED, STORAGE, STORAGE};
static const VkDescriptorType bindings_downsample[] = {COMBINED, STORAGE};
static const VkDescriptorType bindings_ingest[] = {SAMPLED, STORAGE, STORAGE};

#define PASS(name, spv, push)                                                                     \
    {spv, spv##_size, bindings_##name, (uint32_t)(sizeof bindings_##name / sizeof(VkDescriptorType)), push}

const struct pass_desc passes[PASS_COUNT] = {
    [PASS_PREPARE_LUMA] = PASS(prepare, ffx_opticalflow_prepare_luma_pass_spv, 0),
    [PASS_PYRAMID] = PASS(pyramid, ffx_opticalflow_compute_luminance_pyramid_pass_spv, 0),
    [PASS_SCD_HISTOGRAM] = PASS(scd_histogram, ffx_opticalflow_generate_scd_histogram_pass_spv, 0),
    [PASS_SCD_DIVERGENCE] = PASS(scd_divergence, ffx_opticalflow_compute_scd_divergence_pass_spv, 0),
    [PASS_SEARCH] = PASS(search, ffx_opticalflow_compute_optical_flow_advanced_pass_v5_spv, 0),
    [PASS_FILTER] = PASS(filter, ffx_opticalflow_filter_optical_flow_pass_v5_spv, 0),
    [PASS_SCALE] = PASS(scale, ffx_opticalflow_scale_optical_flow_advanced_pass_v5_spv, 0),
    [PASS_INTERPOLATE] = PASS(interpolate, afmf_interpolate_rgba8_spv,
                              (uint32_t)sizeof(struct interpolate_push)),
    [PASS_DOWNSAMPLE] = PASS(downsample, afmf_downsample_rgba8_spv,
                             (uint32_t)sizeof(struct downsample_push)),
    [PASS_INGEST] = PASS(ingest, afmf_ingest_rgba8_spv, (uint32_t)sizeof(struct ingest_push)),
};

const struct downsample_variant downsample_variants[HALF_COUNT] = {
    [HALF_RGBA8] = {afmf_downsample_rgba8_spv, afmf_downsample_rgba8_spv_size, VK_FORMAT_R8G8B8A8_UNORM},
    [HALF_RGBA16F] = {afmf_downsample_rgba16f_spv, afmf_downsample_rgba16f_spv_size,
                      VK_FORMAT_R16G16B16A16_SFLOAT},
};

const struct spirv_variant interpolate_variants[VARIANT_COUNT] = {
    [VARIANT_RGBA8] = {afmf_interpolate_rgba8_spv, afmf_interpolate_rgba8_spv_size},
    [VARIANT_RGBA8_BGRA] = {afmf_interpolate_rgba8_bgra_spv, afmf_interpolate_rgba8_bgra_spv_size},
    [VARIANT_RGB10A2] = {afmf_interpolate_rgb10a2_spv, afmf_interpolate_rgb10a2_spv_size},
    [VARIANT_RGBA16F] = {afmf_interpolate_rgba16f_spv, afmf_interpolate_rgba16f_spv_size},
    [VARIANT_NOFORMAT] = {afmf_interpolate_noformat_spv, afmf_interpolate_noformat_spv_size},
};

/* The fused ingest per ring format; a B8G8R8A8 ring has no SPIR-V format and stores unqualified. */
const struct spirv_variant ingest_variants[VARIANT_COUNT] = {
    [VARIANT_RGBA8] = {afmf_ingest_rgba8_spv, afmf_ingest_rgba8_spv_size},
    [VARIANT_RGBA8_BGRA] = {afmf_ingest_noformat_spv, afmf_ingest_noformat_spv_size},
    [VARIANT_RGB10A2] = {afmf_ingest_rgb10a2_spv, afmf_ingest_rgb10a2_spv_size},
    [VARIANT_RGBA16F] = {afmf_ingest_rgba16f_spv, afmf_ingest_rgba16f_spv_size},
    [VARIANT_NOFORMAT] = {afmf_ingest_noformat_spv, afmf_ingest_noformat_spv_size},
};
