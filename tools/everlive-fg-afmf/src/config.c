#include "config.h"

#include "log.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct afmf_config g_config;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;

/* Reads an integer variable in [min, max] into *out; leaves it untouched when unset. Returns false
 * only when the variable is set to something unparsable or out of range. */
static bool read_bounded(const char *name, long min, long max, long *out)
{
    const char *text = getenv(name);
    if (text == NULL)
        return true;
    if (*text == '\0')
        return false;

    char *end = NULL;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno != 0 || *end != '\0' || value < min || value > max)
        return false;

    *out = value;
    return true;
}

struct choice {
    const char *word;
    long value;
};

/* Reads a word variable against a table of choices; same contract as read_bounded. */
static bool read_choice(const char *name, const struct choice *choices, size_t count, long *out)
{
    const char *text = getenv(name);
    if (text == NULL)
        return true;
    for (size_t i = 0; i < count; i++) {
        if (strcmp(text, choices[i].word) == 0) {
            *out = choices[i].value;
            return true;
        }
    }
    return false;
}

/* Whether this process is Gamescope itself. With AFMF_GAMESCOPE=1 the same environment reaches
 * the compositor and the game it runs, and only the game should get companions. */
static bool process_is_gamescope(void)
{
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0)
        return false;
    exe[n] = '\0';
    const char *base = strrchr(exe, '/');
    return strcmp(base != NULL ? base + 1 : exe, "gamescope") == 0;
}

/* Runs under pthread_once: it must not log, because logging reads the config back. */
static void init(void)
{
    long log_level = AFMF_LOG_ERROR;
    long gamescope = 0;
    bool ok = read_bounded("AFMF_GAMESCOPE", 0, 1, &gamescope);
    /* Measured with vkcube on a 165 Hz Wayland desktop: a free image only comes back once the
     * compositor releases one, about one refresh period after a present. Two extra images make
     * one available by the next present; one extra was not enough. Gamescope keeps more images
     * in flight (direct scanout): with 2 extra a third of the presents found a free one, with 4
     * most, with 5 all of them, and 3 + 5 stays within the 8 some engines accept. The companion's
     * image is acquired a frame ahead (swapchain.c, spare_*), so no wait is needed in the present
     * hook: the timeout only bounds the wait for a spare's release, and 0 means never stall. */
    long extra_images = gamescope != 0 ? 5 : 2;
    long acquire_timeout_us = 0;
    long interpolate = 1;
    /* ADLX search mode: standard keeps the search to 5 pyramid levels (+-128 px), high uses all 7
     * (+-512 px); auto lets the flow resolution decide (framegen.c), which at half resolution is
     * 5 levels, still +-256 px of motion on screen between two frames. */
    long search_mode = AFMF_SEARCH_AUTO;
    long fast_motion = AFMF_RESPONSE_BLENDED_FRAMES;
    /* The flow at half resolution from 1440p up, where the search dominates the frame's cost and
     * a game is most likely to be saturating the GPU already; full resolution below it. */
    long performance = AFMF_PERFORMANCE_AUTO;
    long profile = 0;
    long async = 1;
    /* Pacing holds the real frame back by half a frame time so the generated one lands halfway,
     * as the driver-level implementation does (its documented 4-5 ms of added latency at 120 fps). */
    long pacing = 1;
    /* Every present takes a vblank and the layer doubles the presents: in FIFO a game above half
     * the refresh rate loses real frames (120 at 165 Hz -> 82). MAILBOX drops the excess instead. */
    long present_mode = AFMF_PRESENT_AUTO;
    /* Under GPU contention the generated frame is ready late and the layer's work is taken
     * from the game's budget: the governor steps generation down (fewer search levels, then one
     * companion in two, then in three) and back up as the GPU catches up. Below the minimum real
     * frame rate doubling is not worth its latency. */
    long governor = 1;
    /* Default min_fps lowered to 20 so 29.97/30.00 fps console feeds are never locked out */
    long min_fps = 20;
    /* Sum of absolute 8-bit luma differences over a block's 64 pixels between the two frames at
     * rest; 128 is two levels per pixel on average, what temporal anti-aliasing leaves on a
     * still image. Under it the block is stored as static and its search is skipped. */
    long static_block_sad = 128;
    /* Writing the interpolated frame straight into the swapchain image saves its copy (33 us at
     * 3440x1440) but needs STORAGE usage on every swapchain image, which can cost the game's own
     * rendering more than that (the driver may drop compression on them): off until measured
     * in the game at hand. */
    long direct_output = 0;
    /* A HUD, crosshair or subtitle does not move with the scene: a pixel that is the same in
     * both frames while its block moves is kept instead of warped. */
    long hud_detect = 1;
    /* One read of the game's frame writes the colour ring and the luma; the copy and the SDK's
     * luma pass go. Needs SAMPLED usage on the game's images, which costs the game nothing on
     * RADV; AFMF_DIRECT_INGEST=0 puts the copy back for a driver where it does. */
    long direct_ingest = 1;
    /* The block search's SAD on packed 16-bit pairs (shaderInt16, which the layer enables on the
     * device when the application did not); 0 keeps the SDK's byte-at-a-time code. */
    long sad_int16 = 1;
    /* Frame generation multiplier: 2 (default), 3, or 4. */
    long multiplier = 2;
    /* FidelityFX CAS sharpness: 0..100% (default 40%). */
    long cas_sharpness = 40;
    /* HUD difference threshold: 0..1000 per mille (default 45 = 0.045 for capture signals). */
    long hud_threshold = 45;

    static const struct choice search_modes[] = {{"auto", AFMF_SEARCH_AUTO},
                                                {"standard", AFMF_SEARCH_STANDARD},
                                                {"high", AFMF_SEARCH_HIGH}};
    static const struct choice responses[] = {{"repeat", AFMF_RESPONSE_REPEAT_FRAMES},
                                             {"blend", AFMF_RESPONSE_BLENDED_FRAMES}};
    static const struct choice performance_modes[] = {{"auto", AFMF_PERFORMANCE_AUTO},
                                                     {"quality", AFMF_PERFORMANCE_QUALITY},
                                                     {"performance", AFMF_PERFORMANCE_FAST}};
    static const struct choice present_modes[] = {{"auto", AFMF_PRESENT_AUTO},
                                                 {"keep", AFMF_PRESENT_KEEP}};

    ok = read_bounded("AFMF_LOG", AFMF_LOG_ERROR, AFMF_LOG_DEBUG, &log_level) && ok;
    ok = read_bounded("AFMF_EXTRA_IMAGES", 1, 8, &extra_images) && ok;
    ok = read_bounded("AFMF_ACQUIRE_TIMEOUT_US", 0, 100000, &acquire_timeout_us) && ok;
    ok = read_bounded("AFMF_INTERPOLATE", 0, 1, &interpolate) && ok;
    ok = read_choice("AFMF_SEARCH_MODE", search_modes, 3, &search_mode) && ok;
    ok = read_choice("AFMF_FAST_MOTION_RESPONSE", responses, 2, &fast_motion) && ok;
    ok = read_bounded("AFMF_PROFILE", 0, 1, &profile) && ok;
    ok = read_bounded("AFMF_ASYNC", 0, 1, &async) && ok;
    ok = read_bounded("AFMF_PACING", 0, 1, &pacing) && ok;
    ok = read_choice("AFMF_PERFORMANCE_MODE", performance_modes, 3, &performance) && ok;
    ok = read_choice("AFMF_PRESENT_MODE", present_modes, 2, &present_mode) && ok;
    ok = read_bounded("AFMF_GOVERNOR", 0, 1, &governor) && ok;
    ok = read_bounded("AFMF_MIN_FPS", 0, 240, &min_fps) && ok;
    ok = read_bounded("AFMF_STATIC_BLOCK_SAD", 0, 16320, &static_block_sad) && ok;
    ok = read_bounded("AFMF_DIRECT_OUTPUT", 0, 1, &direct_output) && ok;
    ok = read_bounded("AFMF_HUD_DETECT", 0, 1, &hud_detect) && ok;
    ok = read_bounded("AFMF_DIRECT_INGEST", 0, 1, &direct_ingest) && ok;
    ok = read_bounded("AFMF_SAD_INT16", 0, 1, &sad_int16) && ok;
    ok = read_bounded("AFMF_MULTIPLIER", 2, 4, &multiplier) && ok;
    ok = read_bounded("AFMF_CAS_SHARPNESS", 0, 100, &cas_sharpness) && ok;
    ok = read_bounded("AFMF_HUD_THRESHOLD", 0, 1000, &hud_threshold) && ok;

    g_config.log_level = (int)log_level;
    g_config.extra_images = (uint32_t)extra_images;
    g_config.acquire_timeout_ns = (uint64_t)acquire_timeout_us * 1000u;
    g_config.interpolate = interpolate != 0;
    g_config.search_mode = (enum afmf_search_mode)search_mode;
    g_config.fast_motion = (enum afmf_fast_motion_response)fast_motion;
    g_config.performance = (enum afmf_performance_mode)performance;
    /* Copied: the pointer getenv returns lives in the environment block, which a later setenv
     * (Wine does them) may move. Kept for the life of the process. */
    const char *dump_dir = getenv("AFMF_DUMP_DIR");
    g_config.dump_dir = dump_dir != NULL && *dump_dir != '\0' ? strdup(dump_dir) : NULL;
    g_config.profile = profile != 0 || log_level >= AFMF_LOG_DEBUG;
    g_config.async = async != 0;
    g_config.pacing = pacing != 0;
    g_config.passive = gamescope != 0 && process_is_gamescope();
    g_config.present_mode = (enum afmf_present_mode)present_mode;
    g_config.governor = governor != 0;
    g_config.min_fps = (uint32_t)min_fps;
    g_config.static_block_sad = (uint32_t)static_block_sad;
    g_config.direct_output = direct_output != 0;
    g_config.hud_detect = hud_detect != 0;
    g_config.direct_ingest = direct_ingest != 0;
    g_config.sad_int16 = sad_int16 != 0;
    g_config.multiplier = (uint32_t)multiplier;
    g_config.cas_sharpness = (float)cas_sharpness / 100.0f;
    g_config.hud_threshold = (float)hud_threshold / 1000.0f;
    if (g_config.multiplier >= 4 && g_config.extra_images < 5)
        g_config.extra_images = 5;
    else if (g_config.multiplier >= 3 && g_config.extra_images < 4)
        g_config.extra_images = 4;
    g_config.invalid = !ok;
}

const struct afmf_config *afmf_config_get(void)
{
    (void)pthread_once(&g_once, init); /* only fails on an invalid once_control, which this is not */
    return &g_config;
}
