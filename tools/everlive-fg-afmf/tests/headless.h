#pragma once

/* Headless integration test for VK_LAYER_AFMF: what its translation units share. See
 * headless.c for the whole picture. */

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define LAYER_NAME "VK_LAYER_AFMF"
#define VALIDATION_LAYER_NAME "VK_LAYER_KHRONOS_validation"
#define FRAMES 120u
#define EXIT_SKIP 77

/* Synthetic content: a white square sliding right on black, SQUARE_STEP pixels per frame, so an
 * interpolated frame must show it halfway between two real ones. */
#define SQUARE_SIZE 64u
#define SQUARE_X0 64u
#define SQUARE_Y 200u
#define SQUARE_STEP 8u
#define SQUARE_WRAP 448u
/* The layer dumps its companions from real frame 8 on (the flow's scene change detector reports
 * a change for its first six frames, and the search stores zero vectors while it does). */
#define DUMP_FIRST 8u
/* AFMF_TEST_HUD_BAR=1: a thin red vertical bar that never moves, drawn over the square's path
 * where the dumped frames cross it (square at x 128..216 for frames 8-11), like a crosshair over
 * the scene: its blocks are mostly moving square, so their vectors carry the square's motion and
 * warping would break the bar. The layer's HUD detection must keep it whole. */
#define BAR_X 170u
#define BAR_W 2u
#define BAR_Y0 150u
#define BAR_Y1 350u

#define CHECK(expr)                                                                         \
    do {                                                                                    \
        VkResult check_result_ = (expr);                                                    \
        if (check_result_ != VK_SUCCESS) {                                                  \
            (void)fprintf(stderr, "%s:%d: %s -> VkResult %d\n", __FILE__, __LINE__, #expr,   \
                          (int)check_result_);                                              \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

struct ctx {
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkSurfaceKHR surface;
    VkPhysicalDevice physical_device;
    uint32_t queue_family;
    VkDevice device;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkCommandPool command_pool;
    VkCommandBuffer command_buffer;
    VkSemaphore acquired;
    VkSemaphore ready;
    VkExtent2D extent;
    VkBuffer staging;
    VkDeviceMemory staging_memory;
    uint8_t *staging_mapped;
    uint32_t validation_errors;
    uint32_t frames; /* what the run actually presented, which AFMF_TEST_FRAMES may have changed */
    int present_id; /* AFMF_TEST_PRESENT_ID: 0 none, 1 VK_KHR_present_id, 2 VK_KHR_present_id2 */
    bool present_modes; /* AFMF_TEST_PRESENT_MODES=1: VK_EXT_swapchain_maintenance1 mode list and per-present mode, FIFO */
};

static inline bool env_is_1(const char *name)
{
    const char *v = getenv(name);
    return v != NULL && v[0] == '1';
}

static inline int wanted_present_id(void)
{
    const char *v = getenv("AFMF_TEST_PRESENT_ID");
    return v == NULL ? 0 : v[0] == '2' ? 2 : v[0] == '1' ? 1 : 0;
}

static inline bool wanted_present_modes(void) { return env_is_1("AFMF_TEST_PRESENT_MODES"); }
static inline bool wanted_hud_bar(void) { return env_is_1("AFMF_TEST_HUD_BAR"); }
/* AFMF_TEST_PAN=1: instead of a square on black, a textured picture that scrolls SQUARE_STEP
 * pixels per frame across the whole frame, like a camera pan: every block moves and has to be
 * searched, the worst case for the flow's cost, and every block's vector is known (check_pan). */
static inline bool wanted_pan(void) { return env_is_1("AFMF_TEST_PAN"); }
/* AFMF_TEST_FOG=1: the same scroll with almost no contrast, a soft gradient under noise of a
 * couple of levels. It is the block search's worst case for cost: with no detail to match, every
 * candidate of the 256 matches about as well as the rest, so the rules that let a block skip its
 * search find no evidence and nearly every block searches. No vector is checked here, because in
 * a picture without texture there is no right answer to check against. */
static inline bool wanted_fog(void) { return env_is_1("AFMF_TEST_FOG"); }
/* AFMF_TEST_NO_IDLE=1: no vkDeviceWaitIdle before teardown, where the layer drains its
 * threads; the swapchain is destroyed straight after the last present, with the frame still
 * queued to the work thread or held by the presentation thread, and its destruction has to
 * drain both itself. */
static inline bool wanted_no_idle(void) { return env_is_1("AFMF_TEST_NO_IDLE"); }

/* headless_instance.c */
bool instance_layer_available(const char *name);
bool instance_extension_available(const char *name);
bool device_extension_available(VkPhysicalDevice device, const char *name);
bool create_instance(struct ctx *ctx, bool with_validation);
bool pick_physical_device(struct ctx *ctx);

/* headless_device.c */
bool create_device_and_swapchain(struct ctx *ctx);
void destroy(struct ctx *ctx);
void destroy_swapchain(struct ctx *ctx);

/* headless_swapchain.c */
bool create_swapchain(struct ctx *ctx, uint32_t width, uint32_t height);

/* headless_frame.c */
uint32_t square_x(uint32_t frame);
bool present_frame(struct ctx *ctx, uint32_t frame);

/* headless_checks.c */
bool check_dump(const char *dir, uint32_t n);
bool check_bar(const char *dir, uint32_t n);
bool check_pan(const char *dir, uint32_t n, uint32_t width);
