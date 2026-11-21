/* The golden checks, read back from the layer's AFMF_DUMP_DIR dumps. */

#include "headless.h"

/* Opens the PPM dump of generated frame `n` and reads its header; NULL when unreadable. */
static FILE *open_ppm(const char *dir, uint32_t n, char *path, size_t path_size, unsigned *w, unsigned *h)
{
    if (snprintf(path, path_size, "%s/afmf_generated_%u.ppm", dir, n) >= (int)path_size)
        return NULL;
    FILE *in = fopen(path, "rb");
    if (in == NULL) {
        (void)fprintf(stderr, "no dump at %s: %s\n", path, strerror(errno));
        return NULL;
    }
    unsigned maxval = 0;
    if (fscanf(in, "P6 %u %u %u", w, h, &maxval) == 3 && fgetc(in) == '\n' && *w > 0 && *h > 0 &&
        *w <= 8192 && *h <= 8192)
        return in;
    (void)fclose(in);
    (void)fprintf(stderr, "%s: unreadable\n", path);
    return NULL;
}

/* The flow dump of frame `n`: how many of the blocks fully inside the square in both frames
 * carry the square's vector. `expected_vx` is set from the flow's block size. */
static void count_square_flow(const char *dir, uint32_t n, unsigned w, unsigned long *inside,
                              unsigned long *right, int *expected_vx)
{
    char path[512];
    *inside = *right = 0;
    *expected_vx = 0;
    if (snprintf(path, sizeof path, "%s/afmf_flow_%u.txt", dir, n) >= (int)sizeof path)
        return;
    FILE *in = fopen(path, "r");
    if (in == NULL)
        return;
    unsigned fw = 0, fh = 0;
    if (fscanf(in, "%u %u", &fw, &fh) == 2 && fw > 0 && w % fw == 0) {
        unsigned block = w / fw; /* screen pixels per flow texel: 8, or 16 at half resolution */
        *expected_vx = -(int)(SQUARE_STEP * 8u / block);
        int c;
        while ((c = fgetc(in)) != '\n' && c != EOF)
            ;
        for (unsigned by = 0; by < fh; by++) {
            for (unsigned bx = 0; bx < fw; bx++) {
                int vx = 0, vy = 0;
                if (fscanf(in, "%d %d", &vx, &vy) != 2)
                    break;
                /* Blocks fully inside the square in both frames, one block in from its edges. */
                unsigned px = bx * block, py = by * block;
                if (px < square_x(n) + block || px + block > square_x(n - 1) + SQUARE_SIZE - block ||
                    py < SQUARE_Y + block || py + block > SQUARE_Y + SQUARE_SIZE - block)
                    continue;
                (*inside)++;
                *right += vx == *expected_vx && vy == 0;
            }
        }
    }
    (void)fclose(in);
}

/* Reads the layer's dump of generated frame `n` (between real frames n-1 and n) and checks that
 * the square's centroid sits halfway between where those two frames drew it. */
bool check_dump(const char *dir, uint32_t n)
{
    char path[512];
    unsigned w = 0, h = 0;
    FILE *in = open_ppm(dir, n, path, sizeof path, &w, &h);
    if (in == NULL)
        return false;
    double sum_x = 0, sum_y = 0;
    unsigned long bright = 0;
    bool ok = true;
    for (unsigned y = 0; ok && y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            int r = fgetc(in), g = fgetc(in), b = fgetc(in);
            if (r == EOF || g == EOF || b == EOF) {
                ok = false;
                break;
            }
            if (r > 128 && g > 128 && b > 128) {
                sum_x += x;
                sum_y += y;
                bright++;
            }
        }
    }
    (void)fclose(in);
    if (!ok || bright == 0) {
        (void)fprintf(stderr, "%s: unreadable or no bright pixels\n", path);
        return false;
    }
    double cx = sum_x / (double)bright, cy = sum_y / (double)bright;
    double expected_x = ((double)square_x(n - 1) + (double)square_x(n)) / 2.0 + SQUARE_SIZE / 2.0;
    double expected_y = SQUARE_Y + SQUARE_SIZE / 2.0;
    /* Centroid of pixel indices sits half a pixel left/up of the geometric centre. */
    expected_x -= 0.5;
    expected_y -= 0.5;
    bool placed = cx > expected_x - 2.0 && cx < expected_x + 2.0 && cy > expected_y - 2.0 &&
                  cy < expected_y + 2.0;
    /* The centroid and the count cannot tell a warp from a plain blend of the two frames (a
     * zero flow): their overlap has the same centre. The flow itself can: the square moved
     * SQUARE_STEP to the right, so prev = cur + v gives v = (-SQUARE_STEP, 0) in flow pixels
     * (half of it when the flow runs at half resolution) on the blocks inside it. */
    bool sized = bright > SQUARE_SIZE * SQUARE_SIZE / 2 && bright < SQUARE_SIZE * SQUARE_SIZE * 2;
    unsigned long inside, right;
    int expected_vx;
    count_square_flow(dir, n, w, &inside, &right, &expected_vx);
    bool flowed = inside > 0 && right * 10 >= inside * 8;
    (void)fprintf(stderr,
                  "%s: %lu bright pixels, centroid (%.1f, %.1f), expected (%.1f, %.1f); flow (%d, 0) "
                  "on %lu of %lu inner blocks%s\n",
                  path, bright, cx, cy, expected_x, expected_y, expected_vx, right, inside,
                  placed && sized && flowed ? "" : " MISMATCH");
    return placed && sized && flowed;
}

/* Reads the layer's dump of generated frame `n` and checks that every pixel of the bar is still
 * the bar's colour: nothing of the moving square was warped into it. */
bool check_bar(const char *dir, uint32_t n)
{
    char path[512];
    unsigned w = 0, h = 0;
    FILE *in = open_ppm(dir, n, path, sizeof path, &w, &h);
    if (in == NULL)
        return false;
    unsigned long bar = 0, broken = 0, bright = 0;
    double sum_x = 0;
    bool ok = true;
    for (unsigned y = 0; ok && y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            int r = fgetc(in), g = fgetc(in), b = fgetc(in);
            if (r == EOF || g == EOF || b == EOF) {
                ok = false;
                break;
            }
            if (r > 128 && g > 128 && b > 128) {
                sum_x += x;
                bright++;
            }
            if (y < BAR_Y0 || y >= BAR_Y1 || x < BAR_X || x >= BAR_X + BAR_W)
                continue;
            bar++;
            bool red = r > 128 && g < 64 && b < 64, blue = b > 128 && r < 64 && g < 64;
            if (!red && !blue)
                broken++;
        }
    }
    (void)fclose(in);
    if (!ok || bar == 0) {
        (void)fprintf(stderr, "%s: unreadable\n", path);
        return false;
    }
    (void)fprintf(stderr, "%s: bar %s (%lu of %lu pixels changed); square centre x %.1f, expected %.1f\n",
                  path, broken == 0 ? "intact" : "broken", broken, bar,
                  bright > 0 ? sum_x / (double)bright : 0.0,
                  ((double)square_x(n - 1) + (double)square_x(n)) / 2.0 + SQUARE_SIZE / 2.0 - 0.5);
    return broken == 0;
}

/* With the pan every block moves by SQUARE_STEP: the dumped flow must say so on nearly all of
 * them (the picture's edges have no match and may differ), which is what the search's
 * prediction early-out must not break. */
bool check_pan(const char *dir, uint32_t n, uint32_t width)
{
    char path[512];
    if (snprintf(path, sizeof path, "%s/afmf_flow_%u.txt", dir, n) >= (int)sizeof path)
        return false;
    FILE *in = fopen(path, "r");
    if (in == NULL) {
        (void)fprintf(stderr, "no flow dump at %s: %s\n", path, strerror(errno));
        return false;
    }
    unsigned fw = 0, fh = 0;
    unsigned long blocks = 0, right = 0;
    int expected_vx = 0;
    if (fscanf(in, "%u %u", &fw, &fh) == 2 && fw > 0 && fh > 0) {
        /* Screen pixels per flow texel: 8, or 16 at half resolution. The picture scrolls to the
         * left (what is at x now was at x + SQUARE_STEP before), so prev = cur + v gives +STEP. */
        unsigned block = width / fw;
        expected_vx = block > 0 ? (int)(SQUARE_STEP * 8u / block) : 0;
        int c;
        while ((c = fgetc(in)) != '\n' && c != EOF)
            ;
        for (unsigned by = 0; by < fh; by++) {
            for (unsigned bx = 0; bx < fw; bx++) {
                int vx = 0, vy = 0;
                if (fscanf(in, "%d %d", &vx, &vy) != 2)
                    break;
                if (bx < 2 || bx + 2 >= fw || by < 1 || by + 1 >= fh)
                    continue; /* the edge rows and columns: nothing to match against */
                blocks++;
                right += vx == expected_vx && vy == 0;
            }
        }
    }
    (void)fclose(in);
    bool ok = blocks > 0 && right * 100 >= blocks * 95;
    (void)fprintf(stderr, "%s: flow (%d, 0) on %lu of %lu blocks%s\n", path, expected_vx, right, blocks,
                  ok ? "" : " MISMATCH");
    return ok;
}
