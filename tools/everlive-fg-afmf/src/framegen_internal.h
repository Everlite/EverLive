#pragma once

/* Frame generation, shared between its translation units: the constants, the types and the
 * prototypes of what one file records for another. Nothing here is visible outside
 * src/framegen_*.c. The pass tables and the recording helpers follow in framegen_passes.h and
 * framegen_record.h, included at the end. */

#include "framegen.h"

#include "afmf_spirv.h"
#include "config.h"
#include "log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define AFMF_FLOW_BLOCK 8u
#define AFMF_DUMP_FRAMES 4u
/* The SDK's scene change detector reports a change for its first six frames and the search stores
 * zero vectors while it does: dumps start after that, or they show a plain blend. */
#define AFMF_DUMP_FIRST 8u
/* Timestamps per slot: one at start, one after each stage. Seven pyramid levels emit twenty-six
 * of them, and profiler_mark drops in silence whatever does not fit. */
#define AFMF_PROFILE_QUERIES 48u
#define AFMF_PROFILE_INTERVAL 300u

/* Profiling stages (AFMF_PROFILE=1): GPU time between consecutive timestamps is attributed to
 * the stage that ended at the second one; the search/filter/scale of all levels add up. */
enum stage {
    STAGE_INGEST,
    STAGE_PREPARE,
    STAGE_PYRAMID, /* the luma pyramid and the scene change detector's histogram, side by side */
    STAGE_SEARCH,  /* includes the detector's divergence pass, in front of the coarsest search */
    STAGE_FILTER,
    STAGE_SCALE,
    STAGE_INTERPOLATE,
    STAGE_OUTPUT,
    STAGE_COUNT
};

#define AFMF_LEVELS 7u
#define AFMF_HISTOGRAM_BINS 256u
#define AFMF_HISTOGRAMS_PER_DIM 3u
#define AFMF_HISTOGRAM_SHIFTS 3u
#define AFMF_SCD_SLOTS 3u
#define AFMF_CB_SIZE 32u
#define AFMF_CB_PER_SLOT (AFMF_LEVELS + 2u) /* one per pyramid level, one for the downsampler, one for the detector */
#define AFMF_CB_SCD (AFMF_LEVELS + 1u)      /* the scene change detector's: level-0 luma, level 0 */
#define AFMF_MIN_EXTENT 128u
#define AFMF_MAX_IMAGES 16u /* swapchain images the direct paths keep a view and a set for */
/* Pixels between frames beyond which the flow is not believed: half of what the search reaches
 * at half resolution (five levels, +-256 on screen). Lower, and a fast pan falls back to the
 * blind blend over most of the picture, which is a double image. */
#define AFMF_MAX_TRUSTED_MOTION 128.0f

enum pass {
    PASS_PREPARE_LUMA,
    PASS_PYRAMID,
    PASS_SCD_HISTOGRAM,
    PASS_SCD_DIVERGENCE,
    PASS_SEARCH,
    PASS_FILTER,
    PASS_SCALE,
    PASS_INTERPOLATE,
    PASS_DOWNSAMPLE,
    PASS_INGEST,
    PASS_COUNT
};

/* VARIANT_NOFORMAT stores without a SPIR-V image format (shaderStorageImageWriteWithoutFormat):
 * direct output into a B8G8R8A8 swapchain image, whose format has no SPIR-V equivalent. */
enum variant {
    VARIANT_RGBA8,
    VARIANT_RGBA8_BGRA,
    VARIANT_RGB10A2,
    VARIANT_RGBA16F,
    VARIANT_NOFORMAT,
    VARIANT_COUNT
};

/* Half-resolution colour for the flow: 8-bit unless the source is scRGB half floats. */
enum half_variant { HALF_RGBA8, HALF_RGBA16F, HALF_COUNT };

#define SAMPLED VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE
#define STORAGE VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
#define COMBINED VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
#define UBO VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC

struct image {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
};

enum set_index {
    SET_PREPARE,
    SET_PYRAMID,
    SET_SCD_HISTOGRAM,
    SET_SCD_DIVERGENCE,
    SET_SEARCH,                  /* + level, 7 */
    SET_FILTER = SET_SEARCH + 7, /* + level, 7 */
    SET_SCALE = SET_FILTER + 7,  /* + level - 1, 6 */
    SET_INTERPOLATE = SET_SCALE + 6,
    SET_DOWNSAMPLE,
    SET_COUNT
};

struct secondary {
    VkCommandBuffer cmd;
    bool recorded;
    uint32_t levels;                      /* search levels it was recorded with; re-recorded on change */
    uint8_t stages[AFMF_PROFILE_QUERIES]; /* profiler stages its timestamps close, replayed per use */
    uint32_t stage_count;
};

struct afmf_framegen {
    VkExtent2D extent;
    VkExtent2D of_extent;   /* what the optical flow sees: extent, or half of it in performance mode */
    uint32_t flow_scale;    /* extent / of_extent: 1 or 2 */
    uint32_t levels;        /* pyramid levels the search walks now: 5..max_levels */
    uint32_t max_levels;    /* what the configuration asked for: 5 or 7 (AFMF_LEVELS) */
    struct image color_half; /* of_extent-sized downscale of the new frame; unused when scale is 1 */
    enum half_variant half;
    VkFormat color_format; /* UNORM sibling of the swapchain format: same bytes, no sRGB decode */
    VkFormat out_format;
    VkFormat swapchain_format;
    enum variant variant;
    /* Direct output (AFMF_DIRECT_OUTPUT): the interpolator writes the swapchain image through a
     * storage view per image, with an interpolate set per (parity, image); `output` is unused. */
    bool direct;
    enum variant direct_variant;
    uint32_t target_count;        /* swapchain images handed in, for either direct path */
    VkImageView *target_views;
    VkDescriptorSet *direct_sets; /* [parity * target_count + image] */
    /* Direct ingest: one pass reads the swapchain image (sampled view per image) and writes the
     * colour ring and the level-0 luma; the copy and the SDK's luma pass are skipped. */
    bool ingest_direct;
    enum variant ingest_variant;
    VkImageView *source_views;
    VkDescriptorSet *ingest_sets; /* [parity * target_count + image] */
    uint32_t transfer_function; /* FidelityFX backbuffer transfer function id */
    float min_luminance, max_luminance;

    uint32_t slots;
    uint32_t cb_stride;

    struct image luma[2][AFMF_LEVELS];
    VkExtent2D luma_size[AFMF_LEVELS];
    struct image flow[2][AFMF_LEVELS];
    VkExtent2D flow_size[AFMF_LEVELS];
    struct image flow_out;
    struct image scd_histogram, scd_previous_histogram, scd_temp, scd_output;
    struct image color[2];
    struct image output;
    VkSampler sampler;

    VkBuffer cb;
    VkDeviceMemory cb_memory;
    uint8_t *cb_mapped;

    VkDescriptorPool pool;
    VkDescriptorSet sets[2][SET_COUNT]; /* per frame parity */

    /* Pre-recorded fixed part of a frame, [slot][parity][companion]; NULL records inline. */
    VkCommandPool secondary_pool;
    struct secondary *secondaries;

    bool initialized; /* layouts transitioned and detector cleared in a command buffer */
    uint32_t frame_index;

    /* GPU timestamps per stage (AFMF_PROFILE=1). One query range per slot; a slot's results are
     * read back when the slot is reused, i.e. after its fence, so it never blocks. */
    VkQueryPool queries;
    uint32_t *query_count;  /* per slot: timestamps written last time */
    uint8_t *query_stage;   /* per slot x AFMF_PROFILE_QUERIES: stage each timestamp closes */
    double stage_ns[STAGE_COUNT];
    uint32_t profiled_frames;

    /* Debug readback of generated frames, only when AFMF_DUMP_DIR is set (8-bit variants). */
    VkBuffer dump_buffer;
    VkDeviceMemory dump_memory;
    uint8_t *dump_mapped;
    VkDeviceSize dump_size;
    uint32_t dumps_written;
    bool dump_recorded; /* the command buffer in flight copies into dump_buffer */
    uint32_t dump_frame;  /* frame index of that dump: the companion of real frame dump_frame */
    uint32_t dump_from;   /* first frame of the batch of four being written */
};

/* std140 layout of cbOF_t / cbOF_SPD_t in ffx_opticalflow_callbacks_glsl.h. */
struct cb_of {
    int32_t input_luma_resolution[2];
    uint32_t pyramid_level;
    uint32_t pyramid_level_count;
    uint32_t frame_index;
    uint32_t backbuffer_transfer_function;
    float min_max_luminance[2];
};

struct cb_spd {
    uint32_t mips;
    uint32_t num_work_groups;
    uint32_t work_group_offset[2];
    uint32_t num_work_groups_pyramid;
    uint32_t pad[3];
};

_Static_assert(sizeof(struct cb_of) == AFMF_CB_SIZE, "cbOF layout");
_Static_assert(sizeof(struct cb_spd) == AFMF_CB_SIZE, "cbOF_SPD layout");

/* framegen_format.c */
bool afmf_framegen_describe_format(VkFormat swapchain_format, struct afmf_framegen *fg);
bool afmf_framegen_format_supports(const struct afmf_device *dev, VkFormat format,
                                   VkFormatFeatureFlags features);
enum variant afmf_framegen_direct_variant_for(const struct afmf_device *dev, VkFormat swapchain_format);
enum variant afmf_framegen_ingest_variant_for(const struct afmf_device *dev, VkFormat swapchain_format);

/* framegen_resources.c */
bool afmf_framegen_find_memory_type(const struct afmf_device *dev, uint32_t type_bits,
                                    VkMemoryPropertyFlags wanted, uint32_t *out);
void afmf_framegen_image_destroy(struct afmf_device *dev, struct image *img);
VkResult afmf_framegen_resources_create(struct afmf_device *dev, struct afmf_framegen *fg);

/* framegen_dump.c */
void afmf_framegen_dump_buffer_create(struct afmf_device *dev, struct afmf_framegen *fg);

/* framegen_profiler.c */
void afmf_framegen_profiler_create(struct afmf_device *dev, struct afmf_framegen *fg);
void afmf_framegen_profiler_collect(struct afmf_device *dev, struct afmf_framegen *fg, uint32_t slot);
void afmf_framegen_profiler_report(struct afmf_framegen *fg);
void afmf_framegen_profiler_mark(struct afmf_device *dev, struct afmf_framegen *fg,
                                 VkCommandBuffer cmd, uint32_t slot, enum stage stage);
void afmf_framegen_profiler_begin(struct afmf_device *dev, struct afmf_framegen *fg,
                                  VkCommandBuffer cmd, uint32_t slot);

/* framegen_descriptors.c */
VkResult afmf_framegen_descriptor_sets_create(struct afmf_device *dev, struct afmf_framegen *fg);

#include "framegen_passes.h"
#include "framegen_record.h"
