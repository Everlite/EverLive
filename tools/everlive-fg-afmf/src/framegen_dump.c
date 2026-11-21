/* Frame generation: the AFMF_DUMP_DIR readback of generated frames, their flow and their luma
 * (8-bit variants). */

#include "framegen_internal.h"

/* The dump writes 8-bit PPM out of a four-byte-per-pixel readback: the two 8-bit variants as they
 * are, the 10-bit one by taking the top bits of each channel. */
static bool dump_writable(enum variant variant)
{
    return variant == VARIANT_RGBA8 || variant == VARIANT_RGBA8_BGRA || variant == VARIANT_RGB10A2;
}

/* Host-visible readback buffer for AFMF_DUMP_DIR; a failure here only disables the dumps. */
void afmf_framegen_dump_buffer_create(struct afmf_device *dev, struct afmf_framegen *fg)
{
    const char *dir = afmf_config_get()->dump_dir;
    if (dir == NULL)
        return;
    /* Four bytes per pixel in the readback, whatever the writer then makes of them: the half-float
     * formats would need twice the buffer and are left out. */
    if (!dump_writable(fg->variant)) {
        AFMF_ERR("AFMF_DUMP_DIR is set but this swapchain's format cannot be dumped; "
                 "no frames will be written");
        return;
    }

    /* The directory is the user's to name, not to create: a dump that writes nothing because the
     * path does not exist yet is a diagnosis lost. Only the last component, as mkdir -p would
     * not: a typo in the middle of the path should still be an error. */
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        AFMF_ERR("AFMF_DUMP_DIR %s cannot be created (%s); no frames will be written", dir,
                 strerror(errno));
        return;
    }

    /* The generated frame, then the level-0 flow after the filter and straight from the search
     * (rg16i, one texel per block), the scene change detector's three words, and the level-0
     * luma of both frames (r8ui). */
    VkDeviceSize size = (VkDeviceSize)fg->extent.width * fg->extent.height * 4u +
                        2u * (VkDeviceSize)fg->flow_size[0].width * fg->flow_size[0].height * 4u +
                        AFMF_SCD_SLOTS * 4u +
                        2u * (VkDeviceSize)fg->luma_size[0].width * fg->luma_size[0].height;
    fg->dump_size = size;
    VkBufferCreateInfo buffer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (dev->fns.create_buffer(dev->handle, &buffer, NULL, &fg->dump_buffer) != VK_SUCCESS)
        return;
    VkMemoryRequirements reqs;
    dev->fns.get_buffer_memory_requirements(dev->handle, fg->dump_buffer, &reqs);
    uint32_t type;
    if (!afmf_framegen_find_memory_type(dev, reqs.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                        &type))
        return;
    VkMemoryAllocateInfo alloc = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = reqs.size,
        .memoryTypeIndex = type,
    };
    void *mapped = NULL;
    if (dev->fns.allocate_memory(dev->handle, &alloc, NULL, &fg->dump_memory) != VK_SUCCESS ||
        dev->fns.bind_buffer_memory(dev->handle, fg->dump_buffer, fg->dump_memory, 0) != VK_SUCCESS ||
        dev->fns.map_memory(dev->handle, fg->dump_memory, 0, size, 0, &mapped) != VK_SUCCESS)
        return;
    fg->dump_mapped = mapped;
}

/* Four frames are written at the start of every swapchain, which in most games is a loading
 * screen. `touch <AFMF_DUMP_DIR>/dump-now` while playing arms another four from the next frame,
 * so they come from the scene on screen. The file is polled once a second on the work thread
 * (one failed remove() per second) rather than watched. */
void afmf_framegen_dump_arm(struct afmf_framegen *fg)
{
    if (fg->dump_mapped == NULL || fg->dumps_written < AFMF_DUMP_FRAMES ||
        fg->frame_index % 60u != 0u)
        return;
    char path[512];
    int n = snprintf(path, sizeof path, "%s/dump-now", afmf_config_get()->dump_dir);
    if (n < 0 || (size_t)n >= sizeof path || remove(path) != 0)
        return;
    fg->dumps_written = 0;
    fg->dump_from = fg->frame_index + 1u;
    AFMF_INFO("dump armed: the next %u generated frames go to %s", AFMF_DUMP_FRAMES,
              afmf_config_get()->dump_dir);
}

bool afmf_framegen_dump_pending(const struct afmf_framegen *fg)
{
    return fg->dump_mapped != NULL && fg->frame_index >= fg->dump_from &&
           fg->dumps_written < AFMF_DUMP_FRAMES;
}

bool afmf_framegen_dump_recorded(const struct afmf_framegen *fg)
{
    return fg->dump_recorded;
}

void afmf_framegen_dump_write(struct afmf_device *dev, struct afmf_framegen *fg)
{
    (void)dev;
    if (!fg->dump_recorded)
        return;
    fg->dump_recorded = false;
    fg->dumps_written++;

    /* The readback memory is host-visible but uncached: one sequential copy out of it, then
     * everything below reads ordinary memory (byte by byte from the mapping it took ~200 ms). */
    uint8_t *data = malloc(fg->dump_size);
    if (data == NULL)
        return;
    memcpy(data, fg->dump_mapped, fg->dump_size);

    char path[512];
    int n = snprintf(path, sizeof path, "%s/afmf_generated_%u.ppm", afmf_config_get()->dump_dir,
                     fg->dump_frame);
    FILE *out = n < 0 || (size_t)n >= sizeof path ? NULL : fopen(path, "wb");
    if (out == NULL) {
        AFMF_ERR("cannot write %s (%s)", path, strerror(errno));
        free(data);
        return;
    }
    uint32_t w = fg->extent.width, h = fg->extent.height;
    (void)fprintf(out, "P6\n%u %u\n255\n", w, h);
    /* The buffer holds what the swapchain sees: RGBA, BGRA when the shader swapped for a
     * B8G8R8A8 target (undone here), or A2B10G10R10 packed into a little-endian word, whose ten
     * bits per channel are cut down to the PPM's eight. */
    bool bgra = fg->variant == VARIANT_RGBA8_BGRA;
    bool packed10 = fg->variant == VARIANT_RGB10A2;
    uint8_t *row = malloc((size_t)w * 3);
    for (uint32_t y = 0; row != NULL && y < h; y++) {
        const uint8_t *px = data + (size_t)y * w * 4;
        for (uint32_t x = 0; x < w; x++, px += 4) {
            if (packed10) {
                uint32_t v = (uint32_t)px[0] | ((uint32_t)px[1] << 8) | ((uint32_t)px[2] << 16) |
                             ((uint32_t)px[3] << 24);
                row[x * 3] = (uint8_t)((v >> 2) & 0xffu);
                row[x * 3 + 1] = (uint8_t)((v >> 12) & 0xffu);
                row[x * 3 + 2] = (uint8_t)((v >> 22) & 0xffu);
                continue;
            }
            row[x * 3] = px[bgra ? 2 : 0];
            row[x * 3 + 1] = px[1];
            row[x * 3 + 2] = px[bgra ? 0 : 2];
        }
        (void)fwrite(row, 1, (size_t)w * 3, out);
    }
    free(row);
    if (fclose(out) != 0)
        AFMF_WARN("error writing %s", path);
    else
        AFMF_INFO("generated frame written to %s", path);

    /* The flow that made it: one "vx vy" pair per block, flow-resolution pixels, prev = cur + v. */
    n = snprintf(path, sizeof path, "%s/afmf_flow_%u.txt", afmf_config_get()->dump_dir,
                 fg->dump_frame);
    out = n < 0 || (size_t)n >= sizeof path ? NULL : fopen(path, "w");
    if (out == NULL) {
        AFMF_ERR("cannot write %s (%s)", path, strerror(errno));
        free(data);
        return;
    }
    uint32_t fw = fg->flow_size[0].width, fh = fg->flow_size[0].height;
    uint32_t lw = fg->luma_size[0].width, lh = fg->luma_size[0].height;
    const uint8_t *base = data + (size_t)w * h * 4;
    const int16_t *flow = (const int16_t *)(const void *)base;
    const int16_t *raw = (const int16_t *)(const void *)(base + (size_t)fw * fh * 4);
    const uint32_t *scd = (const uint32_t *)(const void *)(base + (size_t)fw * fh * 8);
    const uint8_t *luma_cur = base + (size_t)fw * fh * 8 + AFMF_SCD_SLOTS * 4;
    const uint8_t *luma_prev = luma_cur + (size_t)lw * lh;
    (void)fprintf(out, "%u %u scd %u %u %u\n", fw, fh, scd[0], scd[1], scd[2]);
    uint32_t moving = 0, moving_raw = 0;
    for (uint32_t y = 0; y < fh; y++) {
        for (uint32_t x = 0; x < fw; x++) {
            size_t i = ((size_t)y * fw + x) * 2;
            moving += flow[i] != 0 || flow[i + 1] != 0;
            moving_raw += raw[i] != 0 || raw[i + 1] != 0;
            (void)fprintf(out, "%d %d%s", flow[i], flow[i + 1], x + 1 < fw ? "  " : "\n");
        }
    }
    (void)fclose(out);
    double sum_cur = 0, sum_prev = 0;
    uint32_t differing = 0;
    for (size_t i = 0; i < (size_t)lw * lh; i++) {
        sum_cur += luma_cur[i];
        sum_prev += luma_prev[i];
        differing += luma_cur[i] != luma_prev[i];
    }
    float scene_change;
    memcpy(&scene_change, &scd[0], sizeof scene_change); /* the SDK stores the float's bits */
    AFMF_INFO("dump %u: luma %ux%u, mean %.1f now / %.1f before, %u pixels differ; blocks moving: "
              "%u after the filter, %u from the search; scene change %.3f, history bits %u",
              fg->dump_frame, lw, lh, sum_cur / (double)(lw * lh), sum_prev / (double)(lw * lh),
              differing, moving, moving_raw, (double)scene_change, scd[1]);
    free(data);
}
