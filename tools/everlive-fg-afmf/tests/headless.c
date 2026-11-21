/* Headless integration test for VK_LAYER_AFMF.
 *
 * Enables the layer by name (so a missing or broken layer fails vkCreateInstance instead of
 * silently running without it), puts VK_LAYER_KHRONOS_validation below it when available so the
 * calls the layer forwards are validated, creates a swapchain on a VK_EXT_headless_surface and
 * presents FRAMES frames. No window, no environment tricks: built with -DAFMF_SANITIZE=ON it is an
 * ordinary instrumented executable. Exit codes: 0 pass, 1 fail, 77 skipped (no headless surface).
 * The pieces live in the headless_*.c files next to this one; this is the run and its verdict. */

#include "headless.h"

static bool run(struct ctx *ctx)
{
    bool with_validation = instance_layer_available(VALIDATION_LAYER_NAME);
    if (!with_validation)
        (void)fprintf(stderr, "note: " VALIDATION_LAYER_NAME " not installed, running without it\n");

    if (!create_instance(ctx, with_validation) || !pick_physical_device(ctx) ||
        !create_device_and_swapchain(ctx))
        return false;
    /* AFMF_TEST_FRAMES=<n> for longer runs (the layer's profile line comes every 300). */
    uint32_t frames = FRAMES;
    const char *wanted = getenv("AFMF_TEST_FRAMES");
    if (wanted != NULL) {
        long n = strtol(wanted, NULL, 10);
        if (n >= 2 && n <= 100000)
            frames = (uint32_t)n;
    }
    ctx->frames = frames;
    /* AFMF_TEST_RECREATE=<n>: every n frames the swapchain goes down and comes back at another
     * resolution, which is what a game does on a window or setting change and what a run of a
     * fixed size never exercises: the layer has to take its frames apart and build them again
     * while its own threads are still working. */
    uint32_t recreate = 0;
    const char *churn = getenv("AFMF_TEST_RECREATE");
    if (churn != NULL) {
        long n = strtol(churn, NULL, 10);
        if (n >= 2 && n <= 100000)
            recreate = (uint32_t)n;
    }
    static const VkExtent2D sizes[] = {{640, 480}, {800, 600}, {1280, 720}};
    uint32_t size_index = 0;

    struct timespec start, end;
    (void)clock_gettime(CLOCK_MONOTONIC, &start);
    for (uint32_t i = 0; i < frames; i++) {
        if (recreate != 0 && i != 0 && i % recreate == 0) {
            size_index = (size_index + 1) % (uint32_t)(sizeof sizes / sizeof sizes[0]);
            destroy_swapchain(ctx);
            if (!create_swapchain(ctx, sizes[size_index].width, sizes[size_index].height))
                return false;
            (void)fprintf(stderr, "swapchain recreated at %ux%u after %u frames\n",
                          ctx->extent.width, ctx->extent.height, i);
        }
        if (!present_frame(ctx, i))
            return false;
    }
    (void)clock_gettime(CLOCK_MONOTONIC, &end);
    double ms = ((double)(end.tv_sec - start.tv_sec) * 1e3 + (double)(end.tv_nsec - start.tv_nsec) / 1e6);
    (void)fprintf(stderr, "%ux%u: %.2f ms per present (upload + layer work, queue drained each frame)\n",
                  ctx->extent.width, ctx->extent.height, ms / frames);
    /* AFMF_TEST_MAX_MS=<n>: a ceiling on that figure. It is a blunt instrument, since the figure
     * carries this test's own upload and a queue drain per frame as well as the layer's work, so
     * it is set with room to spare: what it is there to catch is the class of change that
     * doubles the layer's GPU cost, which is the one a player feels. The layer's own per-stage
     * breakdown (AFMF_LOG=2 AFMF_PROFILE=1) is the number to read when this fails. */
    const char *ceiling = getenv("AFMF_TEST_MAX_MS");
    if (ceiling != NULL) {
        double max_ms = strtod(ceiling, NULL);
        if (max_ms > 0.0 && ms / frames > max_ms) {
            (void)fprintf(stderr, "headless: FAIL: %.2f ms per present is over the %.2f ms ceiling\n",
                          ms / frames, max_ms);
            return false;
        }
    }
    return true;
}

int main(void)
{
    if (!instance_layer_available(LAYER_NAME)) {
        (void)fprintf(stderr, LAYER_NAME " not found: set VK_ADD_LAYER_PATH to the build's layer/ dir\n");
        return EXIT_FAILURE;
    }
    if (!instance_extension_available(VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME)) {
        (void)fprintf(stderr, "skipped: " VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME " unavailable\n");
        return EXIT_SKIP;
    }

    const char *dump_dir = getenv("AFMF_DUMP_DIR");
    if (dump_dir != NULL && mkdir(dump_dir, 0755) != 0 && errno != EEXIST) {
        (void)fprintf(stderr, "cannot create %s: %s\n", dump_dir, strerror(errno));
        return EXIT_FAILURE;
    }

    struct ctx ctx = {0};
    bool ok = run(&ctx);
    destroy(&ctx);

    /* The first two dumps are the companions of real frames DUMP_FIRST and the next: the square
     * must be halfway and the flow right, or, with the bar over its path (which breaks the
     * square's symmetry), the bar must be whole. */
    if (ok && dump_dir != NULL)
        ok = wanted_pan()       ? check_pan(dump_dir, DUMP_FIRST, ctx.extent.width) &&
                                      check_pan(dump_dir, DUMP_FIRST + 1, ctx.extent.width)
             : wanted_hud_bar() ? check_bar(dump_dir, DUMP_FIRST) && check_bar(dump_dir, DUMP_FIRST + 1)
                                : check_dump(dump_dir, DUMP_FIRST) && check_dump(dump_dir, DUMP_FIRST + 1);

    if (ctx.validation_errors > 0) {
        (void)fprintf(stderr, "%" PRIu32 " validation error(s)\n", ctx.validation_errors);
        return EXIT_FAILURE;
    }
    /* "headless: FAIL" is what CTest's FAIL_REGULAR_EXPRESSION looks for: a pass regex on the
     * layer's report alone would let a failed golden check through. */
    (void)fprintf(stderr, ok ? "presented %u frames through " LAYER_NAME "\n" : "headless: FAIL\n",
                  ctx.frames);
    return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
