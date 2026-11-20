#pragma once

/* Frame generation: the pass tables, their push constants and the per-device pipelines built
 * from them. Included from framegen_internal.h. */

struct pass_desc {
    const uint32_t *spirv;
    size_t spirv_size;
    const VkDescriptorType *bindings;
    uint32_t binding_count;
    uint32_t push_size;
};

struct spirv_variant {
    const uint32_t *spirv;
    size_t spirv_size;
};

struct downsample_variant {
    const uint32_t *spirv;
    size_t spirv_size;
    VkFormat format;
};

/* framegen_passes.c: the tables the pipelines are built from. */
extern const char *const stage_names[STAGE_COUNT];
extern const struct pass_desc passes[PASS_COUNT];
extern const struct downsample_variant downsample_variants[HALF_COUNT];
extern const struct spirv_variant interpolate_variants[VARIANT_COUNT];
extern const struct spirv_variant ingest_variants[VARIANT_COUNT];

struct interpolate_push {
    int32_t size[2];
    int32_t block;
    int32_t fallback;
    float max_motion;
    float flow_scale;
    float hud_threshold;
    float timestep;
    float cas_sharpness;
};

struct downsample_push {
    int32_t size[2];   /* destination */
    int32_t source[2]; /* the frame: with an odd extent it is not exactly twice the destination */
};

struct ingest_push {
    int32_t size[2];
    int32_t transfer_function;
    float min_luminance;
    float max_luminance;
    int32_t luma_half;
};

/* The GLSL side declares the same members in the same order; a member added on one side only
 * would read the next one's bytes. */
_Static_assert(sizeof(struct interpolate_push) == 36, "interpolate push constants");
_Static_assert(sizeof(struct downsample_push) == 16, "downsample push constants");
_Static_assert(sizeof(struct ingest_push) == 24, "ingest push constants");

struct afmf_framegen_pipelines {
    VkDescriptorSetLayout set_layouts[PASS_COUNT];
    VkPipelineLayout layouts[PASS_COUNT];
    VkPipeline pipelines[PASS_COUNT]; /* PASS_INTERPOLATE / PASS_DOWNSAMPLE / PASS_INGEST unused: see below */
    VkPipeline interpolate[VARIANT_COUNT];
    VkPipeline downsample[HALF_COUNT];
    VkPipeline ingest[VARIANT_COUNT];
};

/* framegen_pipelines.c: the device's pipelines, compiled on a thread from vkCreateDevice. */
struct afmf_framegen_pipelines *afmf_framegen_pipelines_get(struct afmf_device *dev);
