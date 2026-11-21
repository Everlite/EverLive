/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared Console Hub OSD renderer.
 *
 * The card deliberately mirrors the Hub's controls (chips, icon buttons and
 * typography) instead of drawing a separate "video" language.  Text and icons
 * are rasterised from the bundled Noto Sans and Font Awesome fonts.
 */

#include "everlive_osd.h"

#include <SDL_ttf.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OSD_FONT_REGULAR "NotoSans-Regular.ttf"
#define OSD_FONT_SEMIBOLD "NotoSans-SemiBold.ttf"
#define OSD_FONT_EXTRABOLD "NotoSans-ExtraBold.ttf"
#define OSD_FONT_MONO "NotoSansMono-SemiBold.ttf"
#define OSD_FONT_ICONS "fa-solid-900.ttf"

static TTF_Font *g_font_regular;
static TTF_Font *g_font_semibold;
static TTF_Font *g_font_extrabold;
static TTF_Font *g_font_mono;
static TTF_Font *g_font_icons;
static bool g_available;
static bool g_initialised;
static char g_font_dir[512];

static float clamp_scale(float scale)
{
	if (scale < 0.60f)
		return 0.60f;
	if (scale > 2.00f)
		return 2.00f;
	return scale;
}

static int iscale(float value)
{
	return (int)lroundf(value);
}

/* Hub measurements are authored in interface pixels; the OSD applies the
 * shared overlay factor once so 720p lands on the Hub's own 36 px rhythm. */
static int uscale(float base, float unit)
{
	return iscale(base * unit);
}

static uint32_t surface_color(SDL_Surface *surface, uint32_t rgb, Uint8 alpha)
{
	return SDL_MapRGBA(surface->format,
			   (Uint8)((rgb >> 16) & 0xff),
			   (Uint8)((rgb >> 8) & 0xff),
			   (Uint8)(rgb & 0xff), alpha);
}

static void fill_rounded(SDL_Surface *surface, int x, int y, int w, int h,
			 int radius, Uint32 color)
{
	SDL_Rect mid;
	int dy;

	if (w <= 0 || h <= 0)
		return;
	if (radius <= 0) {
		SDL_Rect rect = { x, y, w, h };

		SDL_FillRect(surface, &rect, color);
		return;
	}
	if (radius * 2 > h)
		radius = h / 2;
	if (radius * 2 > w)
		radius = w / 2;

	mid.x = x;
	mid.y = y + radius;
	mid.w = w;
	mid.h = h - 2 * radius;
	SDL_FillRect(surface, &mid, color);

	for (dy = 0; dy < radius; dy++) {
		int rem = radius - dy;
		int dx = (int)(sqrt((double)(radius * radius - rem * rem)) + 0.5);
		int inset = radius - dx;
		SDL_Rect row = { x + inset, y + dy, w - 2 * inset, 1 };

		SDL_FillRect(surface, &row, color);
		row.y = y + h - 1 - dy;
		SDL_FillRect(surface, &row, color);
	}
}

static void fill_rounded_corners(SDL_Surface *surface, int x, int y,
				int w, int h, int top_left, int top_right,
				int bottom_right, int bottom_left,
				Uint32 color)
{
	int row;

	if (w <= 0 || h <= 0)
		return;
	if (top_left == top_right && top_right == bottom_right &&
	    bottom_right == bottom_left) {
		fill_rounded(surface, x, y, w, h, top_left, color);
		return;
	}
	if (top_left < 0)
		top_left = 0;
	if (top_right < 0)
		top_right = 0;
	if (bottom_right < 0)
		bottom_right = 0;
	if (bottom_left < 0)
		bottom_left = 0;
	if (top_left > h / 2)
		top_left = h / 2;
	if (top_right > h / 2)
		top_right = h / 2;
	if (bottom_right > h / 2)
		bottom_right = h / 2;
	if (bottom_left > h / 2)
		bottom_left = h / 2;
	if (top_left > w / 2)
		top_left = w / 2;
	if (top_right > w / 2)
		top_right = w / 2;
	if (bottom_right > w / 2)
		bottom_right = w / 2;
	if (bottom_left > w / 2)
		bottom_left = w / 2;

	for (row = 0; row < h; row++) {
		int left = 0;
		int right = 0;

		if (row < top_left) {
			int dy = top_left - row - 1;

			left = top_left - (int)(sqrt((double)(top_left * top_left -
							    dy * dy)) + 0.5);
		} else if (row >= h - bottom_left) {
			int dy = row - (h - bottom_left);

			left = bottom_left -
			       (int)(sqrt((double)(bottom_left * bottom_left -
						  dy * dy)) + 0.5);
		}
		if (row < top_right) {
			int dy = top_right - row - 1;

			right = top_right -
				(int)(sqrt((double)(top_right * top_right -
						   dy * dy)) + 0.5);
		} else if (row >= h - bottom_right) {
			int dy = row - (h - bottom_right);

			right = bottom_right -
				(int)(sqrt((double)(bottom_right * bottom_right -
						   dy * dy)) + 0.5);
		}
		if (left < 0)
			left = 0;
		if (right < 0)
			right = 0;
		if (left + right >= w)
			continue;
		SDL_FillRect(surface,
			     &(SDL_Rect){ x + left, y + row, w - left - right, 1 },
			     color);
	}
}

static void fill_circle(SDL_Surface *surface, int cx, int cy, int radius,
			Uint32 color)
{
	int dy;

	if (radius <= 0)
		return;
	for (dy = -radius; dy <= radius; dy++) {
		int dx = (int)(sqrt((double)(radius * radius - dy * dy)) + 0.5);
		SDL_Rect row = { cx - dx, cy + dy, dx * 2 + 1, 1 };

		SDL_FillRect(surface, &row, color);
	}
}

static void blit_surface(SDL_Surface *dst, SDL_Surface *src, int x, int y)
{
	SDL_Rect pos = { x, y, 0, 0 };

	if (!dst || !src)
		return;
	SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_BLEND);
	SDL_BlitSurface(src, NULL, dst, &pos);
	SDL_FreeSurface(src);
}

static SDL_Surface *render_text(const char *text, TTF_Font *font, int size,
				uint32_t rgb, Uint8 alpha)
{
	SDL_Color color = {
		(Uint8)((rgb >> 16) & 0xff),
		(Uint8)((rgb >> 8) & 0xff),
		(Uint8)(rgb & 0xff),
		alpha,
	};

	if (!text || !*text || !font)
		return NULL;
	if (TTF_SetFontSize(font, size) < 0)
		return NULL;
	return TTF_RenderUTF8_Blended(font, text, color);
}

static SDL_Surface *render_glyph(uint32_t codepoint, int size, uint32_t rgb,
				 Uint8 alpha)
{
	SDL_Color color = {
		(Uint8)((rgb >> 16) & 0xff),
		(Uint8)((rgb >> 8) & 0xff),
		(Uint8)(rgb & 0xff),
		alpha,
	};

	if (!g_font_icons || TTF_SetFontSize(g_font_icons, size) < 0)
		return NULL;
	return TTF_RenderGlyph32_Blended(g_font_icons, codepoint, color);
}

static int text_width(const char *text, TTF_Font *font, int size)
{
	int w = 0;
	int h = 0;

	if (!text || !*text || !font)
		return 0;
	if (TTF_SetFontSize(font, size) < 0)
		return 0;
	if (TTF_SizeUTF8(font, text, &w, &h) < 0)
		return 0;
	return w;
}

static int text_height(TTF_Font *font, int size)
{
	if (!font || TTF_SetFontSize(font, size) < 0)
		return size;
	return TTF_FontHeight(font);
}

static int glyph_width(uint32_t codepoint, int size)
{
	int minx = 0, maxx = 0, miny = 0, maxy = 0, advance = 0;

	if (!g_font_icons || TTF_SetFontSize(g_font_icons, size) < 0)
		return size;
	if (TTF_GlyphMetrics32(g_font_icons, codepoint, &minx, &maxx,
			       &miny, &maxy, &advance) < 0)
		return size;
	(void)miny;
	(void)maxy;
	return maxx > minx ? maxx - minx : advance;
}

static void draw_text(SDL_Surface *surface, const char *text, TTF_Font *font,
		      int size, int x, int y, uint32_t rgb, Uint8 alpha,
		      int *out_w)
{
	SDL_Surface *rendered = render_text(text, font, size, rgb, alpha);

	if (!rendered) {
		if (out_w)
			*out_w = 0;
		return;
	}
	if (out_w)
		*out_w = rendered->w;
	blit_surface(surface, rendered, x, y);
}

static int utf8_char_size(unsigned char lead)
{
	if (lead < 0x80)
		return 1;
	if ((lead & 0xe0) == 0xc0)
		return 2;
	if ((lead & 0xf0) == 0xe0)
		return 3;
	if ((lead & 0xf8) == 0xf0)
		return 4;
	return 1;
}

static int text_width_tracked(const char *text, TTF_Font *font, int size,
			      int tracking)
{
	int width = 0;
	bool first = true;
	const char *p = text;

	if (!text || !*text || !font)
		return 0;
	while (*p) {
		char glyph[5] = { 0 };
		int n = utf8_char_size((unsigned char)*p);

		if (n < 1)
			n = 1;
		if (n > 4)
			n = 4;
		memcpy(glyph, p, (size_t)n);
		if (!first)
			width += tracking;
		width += text_width(glyph, font, size);
		first = false;
		p += n;
	}
	return width;
}

static void draw_text_tracked(SDL_Surface *surface, const char *text,
			      TTF_Font *font, int size, int x, int y,
			      uint32_t rgb, Uint8 alpha, int tracking,
			      int *out_w)
{
	const char *p = text;

	if (out_w)
		*out_w = 0;
	if (!text || !*text || !font)
		return;
	while (*p) {
		char glyph[5] = { 0 };
		int n = utf8_char_size((unsigned char)*p);
		int w = 0;

		if (n < 1)
			n = 1;
		if (n > 4)
			n = 4;
		memcpy(glyph, p, (size_t)n);
		draw_text(surface, glyph, font, size, x, y, rgb, alpha, &w);
		x += w + tracking;
		if (out_w)
			*out_w += w + tracking;
		p += n;
	}
	if (out_w && *out_w > 0)
		*out_w -= tracking;
}

static void elide_text(const char *text, TTF_Font *font, int size,
		       int max_width, char *out, size_t out_size)
{
	static const char ellipsis[] = "\xE2\x80\xA6";
	size_t len;
	int ellipsis_w;

	if (!out || out_size == 0)
		return;
	if (!text) {
		out[0] = '\0';
		return;
	}
	snprintf(out, out_size, "%s", text);
	if (max_width <= 0 || text_width(out, font, size) <= max_width)
		return;

	ellipsis_w = text_width(ellipsis, font, size);
	len = strlen(out);
	while (len > 0) {
		do {
			len--;
		} while (len > 0 &&
			 ((unsigned char)out[len] & 0xc0) == 0x80);
		out[len] = '\0';
		if (text_width(out, font, size) + ellipsis_w <= max_width) {
			if (strlen(out) + sizeof(ellipsis) <= out_size)
				strcat(out, ellipsis);
			return;
		}
	}
	out[0] = '\0';
}

static void draw_glyph(SDL_Surface *surface, uint32_t codepoint, int size,
		       int x, int y, uint32_t rgb, Uint8 alpha)
{
	SDL_Surface *rendered = render_glyph(codepoint, size, rgb, alpha);

	if (rendered)
		blit_surface(surface, rendered, x, y);
}

static void draw_chip(SDL_Surface *surface, int x, int y, int w, int h,
		      int radius, uint32_t fill, uint32_t border)
{
	fill_rounded(surface, x, y, w, h, radius,
		     surface_color(surface, border, 255));
	fill_rounded(surface, x + 1, y + 1, w - 2, h - 2,
		     radius > 1 ? radius - 1 : 0,
		     surface_color(surface, fill, 255));
}

static void draw_icon_button(SDL_Surface *surface, int x, int y, int size,
			     int radius, uint32_t glyph, int glyph_size,
			     uint32_t fill, uint32_t border, uint32_t color,
			     bool filled, bool hovered)
{
	int glyph_w = glyph_width(glyph, glyph_size);

	if (filled) {
		draw_chip(surface, x, y, size, size, radius, fill, border);
	} else if (hovered) {
		draw_chip(surface, x, y, size, size, radius,
			  EVERLIVE_OSD_HOVER_BG, EVERLIVE_OSD_HOVER_BORDER);
	}
	draw_glyph(surface, glyph, glyph_size,
		   x + (size - glyph_w) / 2,
		   y + (size - glyph_size) / 2, color, 255);
}

static bool snapshot_hover(const struct everlive_osd_snapshot *snap,
			   enum everlive_osd_action action)
{
	return snap && snap->hover_action == action;
}

static const char *action_label(enum everlive_osd_action action)
{
	switch (action) {
	case EVERLIVE_OSD_ACTION_INPUT_1:
		return "HDMI 1";
	case EVERLIVE_OSD_ACTION_INPUT_2:
		return "HDMI 2";
	case EVERLIVE_OSD_ACTION_ASPECT:
		return "Aspect";
	case EVERLIVE_OSD_ACTION_SCALE:
		return "Scale";
	case EVERLIVE_OSD_ACTION_PIP:
		return "Picture-in-Picture";
	case EVERLIVE_OSD_ACTION_VOLUME:
		return "Volume";
	case EVERLIVE_OSD_ACTION_MUTE:
		return "Mute";
	case EVERLIVE_OSD_ACTION_FULLSCREEN:
		return "Fullscreen";
	case EVERLIVE_OSD_ACTION_RECORD:
		return "Record";
	case EVERLIVE_OSD_ACTION_STATS:
		return "Performance Stats";
	case EVERLIVE_OSD_ACTION_HIDE:
		return "Hide overlay";
	case EVERLIVE_OSD_ACTION_STATS_CLOSE:
		return "Close Stats";
	default:
		return NULL;
	}
}

static const char *action_shortcut(enum everlive_osd_action action)
{
	switch (action) {
	case EVERLIVE_OSD_ACTION_INPUT_1:
		return "1";
	case EVERLIVE_OSD_ACTION_INPUT_2:
		return "2";
	case EVERLIVE_OSD_ACTION_ASPECT:
		return "A";
	case EVERLIVE_OSD_ACTION_SCALE:
		return "L";
	case EVERLIVE_OSD_ACTION_PIP:
		return "P";
	case EVERLIVE_OSD_ACTION_VOLUME:
		return "Up / Down";
	case EVERLIVE_OSD_ACTION_MUTE:
		return "M";
	case EVERLIVE_OSD_ACTION_FULLSCREEN:
		return "F";
	case EVERLIVE_OSD_ACTION_RECORD:
		return "Ctrl+R";
	case EVERLIVE_OSD_ACTION_STATS:
		return "Tab / I";
	case EVERLIVE_OSD_ACTION_HIDE:
		return "O";
	case EVERLIVE_OSD_ACTION_STATS_CLOSE:
		return "Tab / I";
	default:
		return NULL;
	}
}

static const char *action_detail(const struct everlive_osd_card *card,
				 enum everlive_osd_action action)
{
	static const char *const aspects[] = {
		"Auto", "16:9", "4:3", "Stretch"
	};
	int aspect;

	switch (action) {
	case EVERLIVE_OSD_ACTION_INPUT_1:
		return card->input_index == 0 ? "Active" : NULL;
	case EVERLIVE_OSD_ACTION_INPUT_2:
		return card->input_index == 1 ? "Active" : NULL;
	case EVERLIVE_OSD_ACTION_ASPECT:
		aspect = card->aspect_index;
		if (aspect < 0 || aspect > 3)
			aspect = 0;
		return aspects[aspect];
	case EVERLIVE_OSD_ACTION_SCALE:
		return card->scale_integer ? "Integer" : "Bilinear";
	default:
		return NULL;
	}
}

static bool file_exists(const char *path)
{
	return path && *path && access(path, R_OK) == 0;
}

static bool font_path(char *out, size_t size, const char *dir,
		      const char *name)
{
	if (!dir || !*dir)
		return false;
	if (snprintf(out, size, "%s/%s", dir, name) >= (int)size)
		return false;
	return file_exists(out);
}

static const char *font_directory(void)
{
	static const char *const candidates[] = {
		"tools/everlive/fonts",
		"../everlive/fonts",
		"/usr/local/lib/everlive/tools/everlive/fonts",
		"/usr/lib/everlive/tools/everlive/fonts",
		"/usr/share/everlive/fonts",
		NULL,
	};
	const char *env = getenv("EVERLIVE_FONT_DIR");
	char base[512];

	if (env && file_exists(env)) {
		snprintf(g_font_dir, sizeof(g_font_dir), "%s", env);
		return g_font_dir;
	}
	for (size_t i = 0; candidates[i]; i++) {
		if (file_exists(candidates[i])) {
			snprintf(g_font_dir, sizeof(g_font_dir), "%s",
				 candidates[i]);
			return g_font_dir;
		}
	}
	{
		const char *base_path = SDL_GetBasePath();

		if (base_path) {
			snprintf(base, sizeof(base), "%s../everlive/fonts",
				 base_path);
			if (file_exists(base)) {
				snprintf(g_font_dir, sizeof(g_font_dir), "%s",
					 base);
				return g_font_dir;
			}
		}
	}
	return NULL;
}

void everlive_osd_set_font_dir(const char *dir)
{
	if (dir && *dir)
		snprintf(g_font_dir, sizeof(g_font_dir), "%s", dir);
}

int everlive_osd_init(void)
{
	const char *dir;
	char path[768];

	if (g_initialised)
		return g_available ? 0 : -1;
	g_initialised = true;

	if (TTF_Init() < 0) {
		fprintf(stderr, "[OSD] SDL_ttf init failed: %s\n", TTF_GetError());
		return -1;
	}
	dir = g_font_dir[0] ? g_font_dir : font_directory();
	if (!dir) {
		fprintf(stderr, "[OSD] Font directory not found; using bitmap OSD\n");
		return -1;
	}
	if (!font_path(path, sizeof(path), dir, OSD_FONT_REGULAR) ||
	    !(g_font_regular = TTF_OpenFont(path, 32))) {
		fprintf(stderr, "[OSD] Cannot open %s/%s\n", dir,
			OSD_FONT_REGULAR);
		return -1;
	}
	if (!font_path(path, sizeof(path), dir, OSD_FONT_SEMIBOLD) ||
	    !(g_font_semibold = TTF_OpenFont(path, 32))) {
		fprintf(stderr, "[OSD] Cannot open %s/%s\n", dir,
			OSD_FONT_SEMIBOLD);
		return -1;
	}
	if (!font_path(path, sizeof(path), dir, OSD_FONT_ICONS) ||
	    !(g_font_icons = TTF_OpenFont(path, 32))) {
		fprintf(stderr, "[OSD] Cannot open %s/%s\n", dir,
			OSD_FONT_ICONS);
		return -1;
	}
	/* ExtraBold and Mono were added with the Hub-parity refresh.  Keep old
	 * installs working by falling back to the two original faces. */
	if (font_path(path, sizeof(path), dir, OSD_FONT_EXTRABOLD))
		g_font_extrabold = TTF_OpenFont(path, 32);
	if (font_path(path, sizeof(path), dir, OSD_FONT_MONO))
		g_font_mono = TTF_OpenFont(path, 32);
	if (!g_font_extrabold)
		g_font_extrabold = g_font_semibold;
	if (!g_font_mono)
		g_font_mono = g_font_regular;

	TTF_SetFontHinting(g_font_regular, TTF_HINTING_LIGHT);
	TTF_SetFontHinting(g_font_semibold, TTF_HINTING_LIGHT);
	TTF_SetFontHinting(g_font_extrabold, TTF_HINTING_LIGHT);
	TTF_SetFontHinting(g_font_mono, TTF_HINTING_LIGHT);
	TTF_SetFontHinting(g_font_icons, TTF_HINTING_LIGHT);
	g_available = true;
	return 0;
}

bool everlive_osd_available(void)
{
	return g_available;
}

void everlive_osd_shutdown(void)
{
	if (g_font_regular)
		TTF_CloseFont(g_font_regular);
	if (g_font_semibold)
		TTF_CloseFont(g_font_semibold);
	if (g_font_extrabold && g_font_extrabold != g_font_semibold)
		TTF_CloseFont(g_font_extrabold);
	if (g_font_mono && g_font_mono != g_font_regular)
		TTF_CloseFont(g_font_mono);
	if (g_font_icons)
		TTF_CloseFont(g_font_icons);
	g_font_regular = NULL;
	g_font_semibold = NULL;
	g_font_extrabold = NULL;
	g_font_mono = NULL;
	g_font_icons = NULL;
	g_available = false;
	if (g_initialised)
		TTF_Quit();
	g_initialised = false;
}

bool everlive_osd_build_toast(SDL_Renderer *renderer, const char *text,
			      enum everlive_osd_tone tone, int viewport_h,
			      SDL_Texture **texture, int *width, int *height,
			      int *scale_out)
{
	SDL_Surface *surface;
	SDL_Texture *built;
	float s = clamp_scale((float)viewport_h / 1080.0f);
	const float ss = 4.0f;
	float pu = EVERLIVE_OSD_HUB_SCALE * s * ss;
	int font_size = uscale(EVERLIVE_OSD_TOAST_SIZE, pu);
	int pad_x = uscale(EVERLIVE_OSD_TOAST_PAD_X, pu);
	int gap = uscale(EVERLIVE_OSD_GAP, pu);
	int dot = uscale(EVERLIVE_OSD_TOAST_DOT, pu);
	int radius = uscale(EVERLIVE_OSD_RADIUS_TOAST, pu);
	int border = (int)ss;
	int text_w;
	int text_h;
	int w;
	int h;
	uint32_t bg;
	uint32_t border_color;
	uint32_t fg;

	if (!renderer || !texture || !g_available || !text || !*text)
		return false;
	if (font_size < 11)
		font_size = 11;
	if (border < 1)
		border = 1;
	switch (tone) {
	case EVERLIVE_OSD_TONE_MINT:
		bg = EVERLIVE_OSD_MINT_BG;
		border_color = EVERLIVE_OSD_MINT_BORDER;
		fg = EVERLIVE_OSD_MINT;
		break;
	case EVERLIVE_OSD_TONE_WARN:
		bg = EVERLIVE_OSD_WARN_BG;
		border_color = EVERLIVE_OSD_WARN_BORDER;
		fg = EVERLIVE_OSD_WARN;
		break;
	case EVERLIVE_OSD_TONE_DANGER:
		bg = EVERLIVE_OSD_DANGER_BG;
		border_color = EVERLIVE_OSD_DANGER_BORDER;
		fg = EVERLIVE_OSD_DANGER;
		break;
	case EVERLIVE_OSD_TONE_NEUTRAL:
	default:
		bg = EVERLIVE_OSD_CHIP_BG;
		border_color = EVERLIVE_OSD_CHIP_BORDER;
		fg = EVERLIVE_OSD_TEXT_MUTED;
		break;
	}
	text_w = text_width(text, g_font_semibold, font_size);
	text_h = text_height(g_font_semibold, font_size);
	w = pad_x * 2 + dot + gap + text_w;
	h = uscale(EVERLIVE_OSD_TOAST_H, pu);
	if (h < text_h + pad_x)
		h = text_h + pad_x;
	if (w < 1 || h < 1)
		return false;

	surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
						 SDL_PIXELFORMAT_RGBA32);
	if (!surface)
		return false;
	SDL_FillRect(surface, NULL, 0);
	fill_rounded(surface, 0, 0, w, h, radius,
		     surface_color(surface, border_color, 255));
	fill_rounded(surface, border, border, w - border * 2, h - border * 2,
		     radius > border ? radius - border : 0,
		     surface_color(surface, bg, 255));
	fill_circle(surface, pad_x + dot / 2, h / 2, dot / 2,
		    surface_color(surface, fg, 255));
	draw_text(surface, text, g_font_semibold, font_size,
		  pad_x + dot + gap, (h - text_h) / 2, fg, 255, NULL);

	built = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_FreeSurface(surface);
	if (!built)
		return false;
	SDL_SetTextureBlendMode(built, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(built, SDL_ScaleModeLinear);
	if (*texture)
		SDL_DestroyTexture(*texture);
	*texture = built;
	if (width)
		*width = (w + (int)ss / 2) / (int)ss;
	if (height)
		*height = (h + (int)ss / 2) / (int)ss;
	if (scale_out)
		*scale_out = (int)(s * 1000.0f + 0.5f);
	return true;
}

void everlive_osd_destroy_card(struct everlive_osd_card *card)
{
	if (!card)
		return;
	if (card->texture)
		SDL_DestroyTexture(card->texture);
	if (card->tooltip)
		SDL_DestroyTexture(card->tooltip);
	if (card->details)
		SDL_DestroyTexture(card->details);
	card->texture = NULL;
	card->tooltip = NULL;
	card->details = NULL;
	card->width = 0;
	card->height = 0;
	card->tooltip_width = 0;
	card->tooltip_height = 0;
	card->tooltip_action = EVERLIVE_OSD_ACTION_NONE;
	card->tooltip_scale = 0.0f;
	card->tooltip_key[0] = '\0';
	card->details_width = 0;
	card->details_height = 0;
	card->details_scale = 0.0f;
	card->details_text[0] = '\0';
	card->details_key[0] = '\0';
	card->scale = 0.0f;
	card->hit_count = 0;
	card->cache_key[0] = '\0';
}

void everlive_osd_destroy_popup(struct everlive_osd_popup *popup)
{
	if (!popup)
		return;
	if (popup->texture)
		SDL_DestroyTexture(popup->texture);
	popup->texture = NULL;
	popup->width = 0;
	popup->height = 0;
	popup->kind = EVERLIVE_OSD_POPUP_NONE;
}

static void add_hit(struct everlive_osd_card *card, SDL_Rect rect,
		    enum everlive_osd_action action)
{
	if (card->hit_count >= (int)SDL_arraysize(card->hits))
		return;
	card->hits[card->hit_count].rect = rect;
	card->hits[card->hit_count].action = action;
	card->hit_count++;
}

bool everlive_osd_build_card(SDL_Renderer *renderer,
			     const struct everlive_osd_snapshot *snap,
			     float scale, int viewport_w, int viewport_h,
			     struct everlive_osd_card *out)
{
	char key[768];
	SDL_Surface *surface;
	SDL_Texture *texture;
	Uint32 bg_color;
	Uint32 card_border;
	Uint32 divider_color;
	float s = clamp_scale(scale);
	float u = EVERLIVE_OSD_HUB_SCALE * s;
	int title_size;
	int label_size;
	int icon_size;
	int chevron_size;
	int pad_x;
	int pad_y;
	int gap;
	int info_gap;
	int control_h;
	int control_pad;
	int control_gap;
	int button;
	int icon_gap;
	int radius_card;
	int radius_group;
	int radius_control;
	int title_tracking;
	int dot_size;
	int divider_w;
	int title_h;
	int title_w;
	int input_seg_w = 0;
	int input_group_w = 0;
	int input_divider;
	int aspect_w;
	int scale_w;
	int icon_count;
	int icons_w;
	int info_w;
	int content_w;
	int width;
	int height;
	int max_width;
	int x;
	int icon_x;
	int text_x;
	int text_y;
	int title_x;
	int title_y;
	int chip_y;
	int hover;
	int input_index;
	int aspect_index;
	bool scale_integer;
	bool signal_live;
	const char *title = snap && snap->title ? snap->title : "";
	bool show_input = snap && snap->show_input;

	if (!renderer || !out || !g_available)
		return false;
	if (viewport_w <= 0 || viewport_h <= 0)
		return false;

	input_index = show_input && snap && snap->input_index == 1 ? 1 : 0;
	aspect_index = snap ? snap->aspect_index : 0;
	if (aspect_index < 0 || aspect_index > 3)
		aspect_index = 0;
	scale_integer = snap && snap->scale_integer;
	signal_live = snap && snap->signal_live;

	snprintf(out->details_text, sizeof(out->details_text), "%s",
		 snap && snap->details ? snap->details : "");
	out->input_index = input_index;
	out->aspect_index = aspect_index;
	out->scale_integer = scale_integer;

	hover = snap ? (int)snap->hover_action : 0;
	if (hover == (int)EVERLIVE_OSD_ACTION_INFO)
		hover = (int)EVERLIVE_OSD_ACTION_NONE;

	snprintf(key, sizeof(key),
		 "%s|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%d|%.3f|%dx%d",
		 title, input_index, aspect_index, scale_integer ? 1 : 0,
		 signal_live ? 1 : 0, show_input, snap && snap->pip,
		 snap && snap->muted, snap && snap->fullscreen,
		 snap && snap->recording, snap && snap->stats_active,
		 hover, s, viewport_w, viewport_h);
	if (out->texture && out->scale == s && out->viewport_w == viewport_w &&
	    out->viewport_h == viewport_h && strcmp(out->cache_key, key) == 0)
		return true;

	out->hit_count = 0;

	title_size = uscale(EVERLIVE_OSD_TITLE_SIZE, u);
	label_size = uscale(EVERLIVE_OSD_LABEL_SIZE, u);
	icon_size = uscale(EVERLIVE_OSD_ICON_SIZE, u);
	chevron_size = uscale(EVERLIVE_OSD_CHEVRON_SIZE, u);
	pad_x = uscale(EVERLIVE_OSD_BAR_PAD_X, u);
	pad_y = uscale(EVERLIVE_OSD_BAR_PAD_Y, u);
	gap = uscale(EVERLIVE_OSD_GAP, u);
	info_gap = uscale(EVERLIVE_OSD_INFO_GAP, u);
	control_h = uscale(EVERLIVE_OSD_CONTROL_H, u);
	control_pad = uscale(EVERLIVE_OSD_CONTROL_PAD_X, u);
	control_gap = uscale(EVERLIVE_OSD_CONTROL_GAP, u);
	button = uscale(EVERLIVE_OSD_ICON_BUTTON, u);
	icon_gap = uscale(EVERLIVE_OSD_ICON_GAP, u);
	radius_card = uscale(EVERLIVE_OSD_RADIUS_CARD, u);
	radius_group = uscale(EVERLIVE_OSD_RADIUS_GROUP, u);
	radius_control = uscale(EVERLIVE_OSD_RADIUS_CONTROL, u);
	title_tracking = uscale(EVERLIVE_OSD_TITLE_TRACKING, u);
	dot_size = uscale(EVERLIVE_OSD_DOT_SIZE, u);
	divider_w = uscale(EVERLIVE_OSD_DIVIDER_W, u);
	if (title_size < 12)
		title_size = 12;
	if (label_size < 11)
		label_size = 11;
	if (icon_size < 12)
		icon_size = 12;
	if (chevron_size < 9)
		chevron_size = 9;
	if (pad_x < 10)
		pad_x = 10;
	if (pad_y < 8)
		pad_y = 8;
	if (gap < 6)
		gap = 6;
	if (info_gap < 12)
		info_gap = 12;
	if (control_h < 26)
		control_h = 26;
	if (control_pad < 8)
		control_pad = 8;
	if (control_gap < 5)
		control_gap = 5;
	if (button < 26)
		button = 26;
	if (icon_gap < 5)
		icon_gap = 5;
	if (radius_card < 6)
		radius_card = 6;
	if (radius_group < 3)
		radius_group = 3;
	if (radius_control < 4)
		radius_control = 4;
	if (dot_size < 5)
		dot_size = 5;
	if (divider_w < 1)
		divider_w = 1;

	title_h = text_height(g_font_extrabold, title_size);
	title_w = text_width_tracked(title, g_font_extrabold, title_size,
				     title_tracking);
	if (show_input) {
		int icon_w = glyph_width(EVERLIVE_OSD_ICON_INPUT, icon_size);
		int label_w = text_width("HDMI 1", g_font_semibold, label_size);

		input_seg_w = control_pad * 2 + icon_w + control_gap + label_w;
		input_divider = divider_w;
		input_group_w = input_seg_w * 2 + input_divider;
	}
	{
		int icon_w = glyph_width(EVERLIVE_OSD_ICON_EXPAND, icon_size);
		int label_w = text_width("Stretch", g_font_semibold, label_size);

		aspect_w = control_pad * 2 + icon_w + control_gap + label_w +
			   control_gap + chevron_size;
	}
	{
		int icon_w = glyph_width(EVERLIVE_OSD_ICON_TABLE_CELLS, icon_size);
		int label_w = text_width("Bilinear", g_font_semibold, label_size);

		scale_w = control_pad * 2 + icon_w + control_gap + label_w;
	}
	icon_count = 6 + (show_input ? 1 : 0);
	icons_w = icon_count * button + (icon_count - 1) * icon_gap;

	info_w = dot_size + gap + title_w;
	content_w = info_w + info_gap +
		    (show_input ? input_group_w + gap : 0) +
		    aspect_w + gap +
		    scale_w + gap * 2 +
		    divider_w + gap +
		    icons_w;
	width = pad_x * 2 + content_w;
	max_width = viewport_w - uscale(10.0f, u) * 2;
	if (max_width < 1)
		max_width = viewport_w;
	if (width > max_width)
		width = max_width;
	if (width < 1)
		return false;
	height = pad_y * 2 + control_h;

	surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
						 SDL_PIXELFORMAT_RGBA32);
	if (!surface)
		return false;

	SDL_FillRect(surface, NULL, 0);
	bg_color = surface_color(surface, EVERLIVE_OSD_CARD_BG,
				 EVERLIVE_OSD_CARD_ALPHA);
	card_border = surface_color(surface, EVERLIVE_OSD_CARD_BORDER, 255);
	divider_color = surface_color(surface, EVERLIVE_OSD_DIVIDER, 255);

	fill_rounded(surface, 0, 0, width, height, radius_card, card_border);
	fill_rounded(surface, 1, 1, width - 2, height - 2,
		     radius_card > 1 ? radius_card - 1 : 0, bg_color);

	title_x = pad_x + dot_size + gap;
	title_y = pad_y + (control_h - title_h) / 2;
	chip_y = pad_y;
	{
		int cx = pad_x + dot_size / 2;
		int cy = pad_y + control_h / 2;
		int dot_w = glyph_width(EVERLIVE_OSD_ICON_DOT, dot_size);
		uint32_t dot_color = signal_live ? EVERLIVE_OSD_LIVE :
						   EVERLIVE_OSD_LIVE_OFF;

		draw_glyph(surface, EVERLIVE_OSD_ICON_DOT, dot_size,
			   cx - dot_w / 2, cy - dot_size / 2,
			   dot_color, 255);
	}
	draw_text_tracked(surface, title, g_font_extrabold, title_size,
			  title_x, title_y, EVERLIVE_OSD_TITLE, 255,
			  title_tracking, NULL);
	add_hit(out, (SDL_Rect){ pad_x, chip_y, info_w, control_h },
		EVERLIVE_OSD_ACTION_INFO);

	x = pad_x + info_w + info_gap;
	if (show_input) {
		bool group_hot =
			snapshot_hover(snap, EVERLIVE_OSD_ACTION_INPUT_1) ||
			snapshot_hover(snap, EVERLIVE_OSD_ACTION_INPUT_2);
		int icon_w = glyph_width(EVERLIVE_OSD_ICON_INPUT, icon_size);
		int i;

		if (group_hot) {
			draw_chip(surface, x, chip_y, input_group_w, control_h,
				  radius_group, EVERLIVE_OSD_SEGMENT_BG,
				  EVERLIVE_OSD_HOVER_BORDER);
		} else {
			draw_chip(surface, x, chip_y, input_group_w, control_h,
				  radius_group, EVERLIVE_OSD_SEGMENT_BG,
				  EVERLIVE_OSD_GROUP_BORDER);
		}
		for (i = 0; i < 2; i++) {
			bool active = i == input_index;
			bool hot = snapshot_hover(snap,
						  i == 0 ?
						  EVERLIVE_OSD_ACTION_INPUT_1 :
						  EVERLIVE_OSD_ACTION_INPUT_2);
			uint32_t fill = active ? EVERLIVE_OSD_INPUT_BG :
						 (hot ? EVERLIVE_OSD_HOVER_BG :
							EVERLIVE_OSD_SEGMENT_BG);
			uint32_t color = active ? EVERLIVE_OSD_BLUE_TEXT :
						  (hot ? EVERLIVE_OSD_TITLE :
							 EVERLIVE_OSD_TEXT_MUTED);
			const char *label = i == 0 ? "HDMI 1" : "HDMI 2";
			int label_w = text_width(label, g_font_semibold,
						 label_size);
			int seg_x = x + i * (input_seg_w + input_divider);
			int tl = i == 0 ? (radius_group > 1 ? radius_group - 1 : 0) : 0;
			int tr = i == 0 ? 0 : (radius_group > 1 ? radius_group - 1 : 0);
			int br = i == 0 ? 0 : (radius_group > 1 ? radius_group - 1 : 0);
			int bl = i == 0 ? (radius_group > 1 ? radius_group - 1 : 0) : 0;

			if (active || hot) {
				fill_rounded_corners(surface, seg_x + 1,
						     chip_y + 1,
						     input_seg_w - 2,
						     control_h - 2, tl, tr, br,
						     bl,
						     surface_color(surface, fill,
								   255));
			}
			text_y = chip_y + (control_h -
					   text_height(g_font_semibold,
						       label_size)) / 2;
			if (active) {
				icon_x = seg_x + control_pad;
				draw_glyph(surface, EVERLIVE_OSD_ICON_INPUT,
					   icon_size, icon_x,
					   chip_y + (control_h - icon_size) / 2,
					   color, 255);
				text_x = icon_x + icon_w + control_gap;
			} else {
				text_x = seg_x + (input_seg_w - label_w) / 2;
			}
			draw_text(surface, label, g_font_semibold, label_size,
				  text_x, text_y, color, 255, NULL);
			add_hit(out, (SDL_Rect){ seg_x, chip_y, input_seg_w,
						 control_h },
				i == 0 ? EVERLIVE_OSD_ACTION_INPUT_1 :
					 EVERLIVE_OSD_ACTION_INPUT_2);
		}
		SDL_FillRect(surface,
			     &(SDL_Rect){ x + input_seg_w,
					  chip_y + uscale(6.0f, u), input_divider,
					  control_h - uscale(12.0f, u) },
			     divider_color);
		x += input_group_w + gap;
	}

	{
		static const char *const names[] = {
			"Auto", "16:9", "4:3", "Stretch"
		};
		bool hot = snapshot_hover(snap, EVERLIVE_OSD_ACTION_ASPECT);
		int icon_w = glyph_width(EVERLIVE_OSD_ICON_EXPAND, icon_size);

		if (hot) {
			draw_chip(surface, x, chip_y, aspect_w, control_h,
				  radius_group, EVERLIVE_OSD_HOVER_BG,
				  EVERLIVE_OSD_BLUE_BORDER);
		} else {
			draw_chip(surface, x, chip_y, aspect_w, control_h,
				  radius_group, EVERLIVE_OSD_CHIP_BG,
				  EVERLIVE_OSD_INPUT_BORDER);
		}
		icon_x = x + control_pad;
		draw_glyph(surface, EVERLIVE_OSD_ICON_EXPAND, icon_size,
			   icon_x, chip_y + (control_h - icon_size) / 2,
			   EVERLIVE_OSD_TEXT_MUTED, 255);
		text_x = icon_x + icon_w + control_gap;
		text_y = chip_y + (control_h -
				   text_height(g_font_semibold,
					       label_size)) / 2;
		draw_text(surface, names[aspect_index], g_font_semibold,
			  label_size, text_x, text_y, EVERLIVE_OSD_TITLE, 255,
			  NULL);
		draw_glyph(surface, EVERLIVE_OSD_ICON_CHEVRON, chevron_size,
			   x + aspect_w - control_pad - chevron_size,
			   chip_y + (control_h - chevron_size) / 2,
			   EVERLIVE_OSD_TEXT_MUTED, 255);
		add_hit(out, (SDL_Rect){ x, chip_y, aspect_w, control_h },
			EVERLIVE_OSD_ACTION_ASPECT);
		x += aspect_w + gap;
	}

	{
		bool hot = snapshot_hover(snap, EVERLIVE_OSD_ACTION_SCALE);
		int icon_w = glyph_width(EVERLIVE_OSD_ICON_TABLE_CELLS,
					 icon_size);

		if (hot) {
			draw_chip(surface, x, chip_y, scale_w, control_h,
				  radius_control, EVERLIVE_OSD_HOVER_BG,
				  EVERLIVE_OSD_HOVER_BORDER);
		} else {
			draw_chip(surface, x, chip_y, scale_w, control_h,
				  radius_control, EVERLIVE_OSD_CHIP_BG,
				  EVERLIVE_OSD_GROUP_BORDER);
		}
		icon_x = x + control_pad;
		draw_glyph(surface, EVERLIVE_OSD_ICON_TABLE_CELLS, icon_size,
			   icon_x, chip_y + (control_h - icon_size) / 2,
			   EVERLIVE_OSD_TEXT_MUTED, 255);
		text_x = icon_x + icon_w + control_gap;
		text_y = chip_y + (control_h -
				   text_height(g_font_semibold,
					       label_size)) / 2;
		draw_text(surface, scale_integer ? "Integer" : "Bilinear",
			  g_font_semibold, label_size, text_x, text_y,
			  EVERLIVE_OSD_TITLE, 255, NULL);
		add_hit(out, (SDL_Rect){ x, chip_y, scale_w, control_h },
			EVERLIVE_OSD_ACTION_SCALE);
		x += scale_w + gap * 2;
	}

	{
		SDL_Rect divider = { x, chip_y + uscale(6.0f, u), divider_w,
				     control_h - uscale(12.0f, u) };

		SDL_FillRect(surface, &divider, divider_color);
	}
	x += divider_w + gap;

#define DRAW_ICON_BUTTON(action, glyph, filled, fill, border, active_color, idle_color) \
	do {								\
		bool hot__ = snapshot_hover(snap, (action));		\
		uint32_t icon_color__ = (filled) ? (active_color) :	\
			(hot__ ? EVERLIVE_OSD_TITLE : (idle_color));	\
		draw_icon_button(surface, x, chip_y, button,		\
				 radius_control, (glyph), icon_size,	\
				 (fill), (border), icon_color__,	\
				 (filled), hot__);			\
		add_hit(out, (SDL_Rect){ x, chip_y, button, button },	\
			(action));					\
		x += button + icon_gap;					\
	} while (0)

	DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_RECORD,
			 snap && snap->recording ? EVERLIVE_OSD_ICON_STOP :
						   EVERLIVE_OSD_ICON_RECORD,
			 snap && snap->recording,
			 EVERLIVE_OSD_DANGER_BG, EVERLIVE_OSD_DANGER_BORDER,
			 EVERLIVE_OSD_ON_ACCENT, EVERLIVE_OSD_DANGER);
	if (show_input) {
		DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_PIP,
				 EVERLIVE_OSD_ICON_PIP,
				 snap && snap->pip,
				 EVERLIVE_OSD_BLUE, EVERLIVE_OSD_BLUE_BORDER,
				 EVERLIVE_OSD_BLUE_TEXT, EVERLIVE_OSD_TEXT_MUTED);
	}
	DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_VOLUME,
			 EVERLIVE_OSD_ICON_VOLUME, false, 0, 0,
			 EVERLIVE_OSD_TEXT_MUTED, EVERLIVE_OSD_TEXT_MUTED);
	DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_MUTE,
			 EVERLIVE_OSD_ICON_MUTE,
			 snap && snap->muted,
			 EVERLIVE_OSD_DANGER_BG, EVERLIVE_OSD_DANGER_BORDER,
			 EVERLIVE_OSD_ON_ACCENT, EVERLIVE_OSD_TEXT_MUTED);
	DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_FULLSCREEN,
			 snap && snap->fullscreen ? EVERLIVE_OSD_ICON_MINIMIZE :
						    EVERLIVE_OSD_ICON_MAXIMIZE,
			 false, 0, 0,
			 EVERLIVE_OSD_TITLE, EVERLIVE_OSD_TEXT_MUTED);
	DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_STATS,
			 EVERLIVE_OSD_ICON_CHART,
			 snap && snap->stats_active,
			 EVERLIVE_OSD_BLUE, EVERLIVE_OSD_BLUE_BORDER,
			 EVERLIVE_OSD_BLUE_TEXT, EVERLIVE_OSD_TEXT_MUTED);
	DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_HIDE,
			 EVERLIVE_OSD_ICON_HIDE, false, 0, 0,
			 EVERLIVE_OSD_TEXT_MUTED, EVERLIVE_OSD_TEXT_MUTED);
#undef DRAW_ICON_BUTTON

	texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_FreeSurface(surface);
	if (!texture)
		return false;
	SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);

	if (out->texture)
		SDL_DestroyTexture(out->texture);
	out->texture = texture;
	out->width = width;
	out->height = height;
	out->scale = s;
	out->viewport_w = viewport_w;
	out->viewport_h = viewport_h;
	out->generation++;
	snprintf(out->cache_key, sizeof(out->cache_key), "%s", key);
	if (getenv("EVERLIVE_OSD_DEBUG"))
		fprintf(stderr,
			"[OSD] bar gen=%u scale=%.2f viewport=%dx%d size=%dx%d hits=%d\\n",
			out->generation, s, viewport_w, viewport_h, width,
			height, out->hit_count);
	return true;
}

const struct everlive_osd_hitbox *everlive_osd_stats_hit(
	const struct everlive_osd_stats *stats, int local_x, int local_y)
{
	int i;

	if (!stats)
		return NULL;
	for (i = 0; i < stats->hit_count; i++) {
		if (local_x >= stats->hits[i].rect.x &&
		    local_x < stats->hits[i].rect.x + stats->hits[i].rect.w &&
		    local_y >= stats->hits[i].rect.y &&
		    local_y < stats->hits[i].rect.y + stats->hits[i].rect.h)
			return &stats->hits[i];
	}
	return NULL;
}

void everlive_osd_destroy_stats(struct everlive_osd_stats *stats)
{
	if (!stats)
		return;
	if (stats->texture) {
		SDL_DestroyTexture(stats->texture);
		stats->texture = NULL;
	}
	stats->width = 0;
	stats->height = 0;
	stats->scale = 0.0f;
	stats->viewport_w = 0;
	stats->viewport_h = 0;
	stats->cache_key[0] = '\0';
	stats->hit_count = 0;
	stats->hover_action = EVERLIVE_OSD_ACTION_NONE;
}

int everlive_osd_stats_max_width(int viewport_w, int card_width,
				 float scale)
{
	float s = clamp_scale(scale);
	float u = EVERLIVE_OSD_HUB_SCALE * s;
	int margin = uscale(34.0f, u);
	int gap = uscale(EVERLIVE_OSD_GAP, u);
	int width;

	if (viewport_w <= 0)
		return 0;
	if (margin < 10)
		margin = 10;
	if (gap < 6)
		gap = 6;
	if (card_width > 0)
		width = (viewport_w - card_width) / 2 - margin - gap;
	else
		width = viewport_w / 4;
	if (width < 120)
		width = 120;
	if (width > viewport_w / 2)
		width = viewport_w / 2;
	return width;
}

bool everlive_osd_build_stats(SDL_Renderer *renderer,
			      const char *const *lines, int line_count,
			      int max_width,
			      enum everlive_osd_action hover_action,
			      float scale, int viewport_w, int viewport_h,
			      struct everlive_osd_stats *out)
{
	char text[EVERLIVE_OSD_STATS_MAX_LINES][256];
	char key[768];
	SDL_Surface *surface;
	SDL_Texture *texture;
	float s = clamp_scale(scale);
	float u = EVERLIVE_OSD_HUB_SCALE * s;
	int pad_x = uscale(16.0f, u);
	int pad_y = uscale(14.0f, u);
	int button = uscale(30.0f, u);
	int icon_size = uscale(16.0f, u);
	int radius = uscale(12.0f, u);
	int button_radius = uscale(8.0f, u);
	int line_size = uscale(12.5f, u);
	int line_gap = uscale(4.0f, u);
	int close_gap = uscale(7.0f, u);
	int text_max_w;
	int text_w = 0;
	int line_h;
	int text_h;
	int panel_w;
	int panel_h;
	int text_x;
	int text_y;
	int i;
	uint32_t hash = 2166136261u;

	if (!renderer || !out || !g_available || !lines)
		return false;
	if (viewport_w <= 0 || viewport_h <= 0 || line_count <= 0)
		return false;
	if (line_count > EVERLIVE_OSD_STATS_MAX_LINES)
		line_count = EVERLIVE_OSD_STATS_MAX_LINES;
	if (max_width <= 0)
		max_width = viewport_w / 2;

	if (pad_x < 10)
		pad_x = 10;
	if (pad_y < 8)
		pad_y = 8;
	if (button < 22)
		button = 22;
	if (icon_size < 11)
		icon_size = 11;
	if (radius < 6)
		radius = 6;
	if (button_radius < 4)
		button_radius = 4;
	if (line_size < 10)
		line_size = 10;
	if (line_gap < 2)
		line_gap = 2;
	if (close_gap < 4)
		close_gap = 4;

	text_max_w = max_width - pad_x * 2;
	if (text_max_w < 60)
		text_max_w = 60;
	for (i = 0; i < line_count; i++) {
		const char *line = lines[i] ? lines[i] : "";
		const char *p;

		elide_text(line, g_font_mono, line_size, text_max_w,
			   text[i], sizeof(text[i]));
		for (p = text[i]; *p; p++)
			hash = (hash ^ (unsigned char)*p) * 16777619u;
		hash = (hash ^ '\n') * 16777619u;
		{
			int w = text_width(text[i], g_font_mono, line_size);

			if (w > text_w)
				text_w = w;
		}
	}

	snprintf(key, sizeof(key), "%08x|%d|%d|%d|%.3f|%dx%d",
		 hash, line_count, max_width, (int)hover_action, s,
		 viewport_w, viewport_h);
	if (out->texture && out->scale == s &&
	    out->viewport_w == viewport_w && out->viewport_h == viewport_h &&
	    strcmp(out->cache_key, key) == 0) {
		out->hover_action = hover_action;
		return true;
	}

	line_h = text_height(g_font_mono, line_size);
	text_h = line_count * line_h + (line_count - 1) * line_gap;
	panel_w = text_w + pad_x * 2;
	if (panel_w < button + pad_x * 2)
		panel_w = button + pad_x * 2;
	if (panel_w > max_width)
		panel_w = max_width;
	if (panel_w < 1)
		return false;
	panel_h = pad_y * 2 + button + close_gap + text_h;
	if (panel_h < 1)
		return false;

	surface = SDL_CreateRGBSurfaceWithFormat(0, panel_w, panel_h, 32,
						 SDL_PIXELFORMAT_RGBA32);
	if (!surface)
		return false;
	SDL_FillRect(surface, NULL, 0);
	fill_rounded(surface, 0, 0, panel_w, panel_h, radius,
		     surface_color(surface, EVERLIVE_OSD_CARD_BORDER, 255));
	fill_rounded(surface, 1, 1, panel_w - 2, panel_h - 2,
		     radius > 1 ? radius - 1 : 0,
		     surface_color(surface, EVERLIVE_OSD_CARD_BG,
				   EVERLIVE_OSD_CARD_ALPHA));

	draw_icon_button(surface, panel_w - pad_x - button, pad_y, button,
			 button_radius, EVERLIVE_OSD_ICON_XMARK, icon_size, 0, 0,
			 hover_action == EVERLIVE_OSD_ACTION_STATS_CLOSE ?
				EVERLIVE_OSD_TITLE : EVERLIVE_OSD_TEXT_MUTED,
			 false,
			 hover_action == EVERLIVE_OSD_ACTION_STATS_CLOSE);
	out->hits[0].rect = (SDL_Rect){ panel_w - pad_x - button, pad_y,
				       button, button };
	out->hits[0].action = EVERLIVE_OSD_ACTION_STATS_CLOSE;
	out->hit_count = 1;

	text_x = pad_x;
	text_y = pad_y + button + close_gap;
	for (i = 0; i < line_count; i++) {
		draw_text(surface, text[i], g_font_mono, line_size,
			  text_x, text_y, EVERLIVE_OSD_TITLE, 255, NULL);
		text_y += line_h + line_gap;
	}

	texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_FreeSurface(surface);
	if (!texture)
		return false;
	SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);

	if (out->texture)
		SDL_DestroyTexture(out->texture);
	out->texture = texture;
	out->width = panel_w;
	out->height = panel_h;
	out->scale = s;
	out->viewport_w = viewport_w;
	out->viewport_h = viewport_h;
	out->hover_action = hover_action;
	out->generation++;
	snprintf(out->cache_key, sizeof(out->cache_key), "%s", key);
	if (getenv("EVERLIVE_OSD_DEBUG"))
		fprintf(stderr, "[OSD] stats gen=%u scale=%.2f %dx%d hits=%d\n",
			out->generation, s, panel_w, panel_h, out->hit_count);
	return true;
}

void everlive_osd_place_stats(SDL_Rect *dst,
			      const struct everlive_osd_stats *stats,
			      bool bottom, const SDL_Rect *card_dest,
			      bool card_visible, float scale,
			      int win_w, int win_h)
{
	int margin;
	int gap;

	if (!dst || !stats || stats->width <= 0 || stats->height <= 0)
		return;
	if (win_w <= 0 || win_h <= 0)
		return;
	margin = uscale(34.0f, EVERLIVE_OSD_HUB_SCALE * scale);
	if (margin < 10)
		margin = 10;
	gap = uscale(EVERLIVE_OSD_GAP, EVERLIVE_OSD_HUB_SCALE * scale);
	if (gap < 6)
		gap = 6;

	dst->w = stats->width;
	dst->h = stats->height;
	dst->x = win_w - margin - dst->w;
	dst->y = bottom ? win_h - margin - dst->h : margin;
	if (card_visible && card_dest) {
		int min_x = card_dest->x + card_dest->w + gap;

		if (dst->x < min_x)
			dst->x = min_x;
	}
	if (dst->x + dst->w > win_w - 8)
		dst->x = win_w - dst->w - 8;
	if (dst->x < 8)
		dst->x = 8;
	if (dst->y < 8)
		dst->y = 8;
	if (dst->y + dst->h > win_h - 8)
		dst->y = win_h - dst->h - 8;
	if (dst->y < 8)
		dst->y = 8;
}

void everlive_osd_render_stats(SDL_Renderer *renderer,
			       const struct everlive_osd_stats *stats,
			       const SDL_Rect *dest, int win_w, int win_h)
{
	SDL_Rect draw;
	int out_w = 0;
	int out_h = 0;

	if (!renderer || !stats || !stats->texture || !dest)
		return;
	if (win_w <= 0 || win_h <= 0)
		return;
	draw = *dest;
	if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) == 0 &&
	    out_w > 0 && out_h > 0) {
		draw.x = lroundf((float)dest->x * (float)out_w / (float)win_w);
		draw.y = lroundf((float)dest->y * (float)out_h / (float)win_h);
		draw.w = lroundf((float)dest->w * (float)out_w / (float)win_w);
		draw.h = lroundf((float)dest->h * (float)out_h / (float)win_h);
	}
	SDL_RenderCopy(renderer, stats->texture, NULL, &draw);
}

bool everlive_osd_build_tooltip(SDL_Renderer *renderer,
				struct everlive_osd_card *card,
				enum everlive_osd_action action,
				float scale)
{
	SDL_Surface *surface;
	SDL_Texture *texture;
	const char *label;
	const char *shortcut;
	const char *detail;
	char key[96];
	float s = clamp_scale(scale);
	float u = EVERLIVE_OSD_HUB_SCALE * s;
	int label_size;
	int key_size;
	int pad;
	int gap;
	int key_pad;
	int radius;
	int label_w;
	int label_h;
	int detail_w = 0;
	int detail_h = 0;
	int key_w;
	int key_h;
	int cap_h;
	int cap_w;
	int content_h;
	int width;
	int height;
	int label_y;
	int detail_x = 0;
	int detail_y = 0;
	int cap_x;
	int cap_y;

	if (!renderer || !card || !g_available)
		return false;
	if (action <= EVERLIVE_OSD_ACTION_NONE ||
	    action >= EVERLIVE_OSD_ACTION_COUNT)
		return false;
	label = action_label(action);
	shortcut = action_shortcut(action);
	detail = action_detail(card, action);
	if (!label || !shortcut)
		return false;
	snprintf(key, sizeof(key), "%d|%s|%.3f", (int)action,
		 detail ? detail : "", s);
	if (card->tooltip && card->tooltip_action == action &&
	    card->tooltip_scale == s && strcmp(card->tooltip_key, key) == 0)
		return true;

	label_size = uscale(EVERLIVE_OSD_TOOLTIP_SIZE, u);
	key_size = uscale(EVERLIVE_OSD_KEY_SIZE, u);
	if (label_size < 11)
		label_size = 11;
	if (key_size < 10)
		key_size = 10;
	pad = uscale(EVERLIVE_OSD_TOOLTIP_PAD, u);
	gap = uscale(EVERLIVE_OSD_TOOLTIP_GAP, u);
	key_pad = uscale(EVERLIVE_OSD_KEY_PAD_X, u);
	radius = uscale(EVERLIVE_OSD_RADIUS_TOOLTIP, u);
	label_w = text_width(label, g_font_semibold, label_size);
	label_h = text_height(g_font_semibold, label_size);
	if (detail) {
		detail_w = text_width(detail, g_font_regular, label_size);
		detail_h = text_height(g_font_regular, label_size);
	}
	key_w = text_width(shortcut, g_font_mono, key_size);
	key_h = text_height(g_font_mono, key_size);
	cap_h = key_h + uscale(4.0f, u);
	cap_w = key_w + key_pad * 2;
	content_h = label_h > cap_h ? label_h : cap_h;
	if (detail_h > content_h)
		content_h = detail_h;
	width = pad * 2 + label_w + gap + cap_w;
	if (detail)
		width += detail_w + gap;
	height = pad * 2 + content_h;
	if (width < 1 || height < 1)
		return false;

	surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
						 SDL_PIXELFORMAT_RGBA32);
	if (!surface)
		return false;
	SDL_FillRect(surface, NULL, 0);
	fill_rounded(surface, 0, 0, width, height, radius,
		     surface_color(surface, EVERLIVE_OSD_TOOLTIP_BORDER, 255));
	fill_rounded(surface, 1, 1, width - 2, height - 2,
		     radius > 1 ? radius - 1 : 0,
		     surface_color(surface, EVERLIVE_OSD_TOOLTIP_BG, 250));
	label_y = (height - label_h) / 2;
	draw_text(surface, label, g_font_semibold, label_size, pad, label_y,
		  EVERLIVE_OSD_TITLE, 255, NULL);
	detail_x = pad + label_w + gap;
	if (detail) {
		detail_y = (height - detail_h) / 2;
		draw_text(surface, detail, g_font_regular, label_size,
			  detail_x, detail_y, EVERLIVE_OSD_TEXT_MUTED, 255,
			  NULL);
	}
	cap_x = detail ? detail_x + detail_w + gap : pad + label_w + gap;
	cap_y = (height - cap_h) / 2;
	draw_chip(surface, cap_x, cap_y, cap_w, cap_h, uscale(6.0f, u),
		  EVERLIVE_OSD_KBD_BG, EVERLIVE_OSD_GROUP_BORDER);
	draw_text(surface, shortcut, g_font_mono, key_size,
		  cap_x + key_pad, cap_y + (cap_h - key_h) / 2,
		  EVERLIVE_OSD_TITLE, 255, NULL);

	texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_FreeSurface(surface);
	if (!texture)
		return false;
	SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	if (card->tooltip)
		SDL_DestroyTexture(card->tooltip);
	card->tooltip = texture;
	card->tooltip_width = width;
	card->tooltip_height = height;
	card->tooltip_action = action;
	card->tooltip_scale = s;
	snprintf(card->tooltip_key, sizeof(card->tooltip_key), "%s", key);
	return true;
}

void everlive_osd_render_tooltip(SDL_Renderer *renderer,
				 const struct everlive_osd_card *card,
				 const SDL_Rect *anchor,
				 int viewport_w, int viewport_h)
{
	SDL_Rect dst;
	SDL_Rect scaled;
	int gap;
	int margin;
	int out_w = 0;
	int out_h = 0;

	if (!renderer || !card || !card->tooltip || !anchor)
		return;
	if (viewport_w <= 0 || viewport_h <= 0)
		return;
	gap = iscale(8.0f * (card->tooltip_scale > 0.0f ?
			     card->tooltip_scale : 1.0f));
	margin = iscale(8.0f * (card->tooltip_scale > 0.0f ?
				card->tooltip_scale : 1.0f));
	dst.w = card->tooltip_width;
	dst.h = card->tooltip_height;
	dst.x = anchor->x + (anchor->w - dst.w) / 2;
	dst.y = anchor->y + anchor->h + gap;
	if (dst.x < margin)
		dst.x = margin;
	if (dst.x + dst.w > viewport_w - margin)
		dst.x = viewport_w - dst.w - margin;
	if (dst.y + dst.h > viewport_h - margin) {
		dst.y = anchor->y - dst.h - gap;
		if (dst.y < margin)
			dst.y = margin;
	}

	scaled = dst;
	if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) == 0 &&
	    out_w > 0 && out_h > 0) {
		scaled.x = lroundf((float)dst.x * (float)out_w /
				   (float)viewport_w);
		scaled.y = lroundf((float)dst.y * (float)out_h /
				   (float)viewport_h);
		scaled.w = lroundf((float)dst.w * (float)out_w /
				   (float)viewport_w);
		scaled.h = lroundf((float)dst.h * (float)out_h /
				   (float)viewport_h);
	}
	SDL_RenderCopy(renderer, card->tooltip, NULL, &scaled);
}

bool everlive_osd_build_details(SDL_Renderer *renderer,
				struct everlive_osd_card *card,
				float scale)
{
	SDL_Surface *surface;
	SDL_Texture *texture;
	char key[224];
	float s = clamp_scale(scale);
	const float ss = 4.0f;
	float pu = EVERLIVE_OSD_HUB_SCALE * s * ss;
	int font_size;
	int pad_x;
	int pad_y;
	int radius;
	int border;
	int text_h;
	int text_w;
	int width;
	int height;

	if (!renderer || !card || !g_available || !card->details_text[0])
		return false;
	snprintf(key, sizeof(key), "%s|%.3f", card->details_text, s);
	if (card->details && card->details_scale == s &&
	    strcmp(card->details_key, key) == 0)
		return true;

	font_size = uscale(EVERLIVE_OSD_DETAIL_SIZE, pu);
	if (font_size < 11)
		font_size = 11;
	pad_x = uscale(EVERLIVE_OSD_DETAIL_PAD_X, pu);
	pad_y = uscale(EVERLIVE_OSD_DETAIL_PAD_Y, pu);
	radius = uscale(EVERLIVE_OSD_RADIUS_CONTROL, pu);
	border = (int)ss;
	if (border < 1)
		border = 1;
	text_h = text_height(g_font_mono, font_size);
	text_w = text_width(card->details_text, g_font_mono, font_size);
	width = pad_x * 2 + text_w;
	height = pad_y * 2 + text_h;
	if (height < uscale(EVERLIVE_OSD_TOAST_H, pu))
		height = uscale(EVERLIVE_OSD_TOAST_H, pu);
	if (width < 1 || height < 1)
		return false;

	surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
						 SDL_PIXELFORMAT_RGBA32);
	if (!surface)
		return false;
	SDL_FillRect(surface, NULL, 0);
	fill_rounded(surface, 0, 0, width, height, radius,
		     surface_color(surface, EVERLIVE_OSD_CHIP_BORDER, 255));
	fill_rounded(surface, border, border, width - border * 2,
		     height - border * 2,
		     radius > border ? radius - border : 0,
		     surface_color(surface, EVERLIVE_OSD_CHIP_BG, 255));
	draw_text(surface, card->details_text, g_font_mono, font_size,
		  pad_x, (height - text_h) / 2, EVERLIVE_OSD_TEXT_MUTED, 255,
		  NULL);

	texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_FreeSurface(surface);
	if (!texture)
		return false;
	SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
	if (card->details)
		SDL_DestroyTexture(card->details);
	card->details = texture;
	card->details_width = (width + (int)ss / 2) / (int)ss;
	card->details_height = (height + (int)ss / 2) / (int)ss;
	if (card->details_width < 1)
		card->details_width = 1;
	if (card->details_height < 1)
		card->details_height = 1;
	card->details_scale = s;
	snprintf(card->details_key, sizeof(card->details_key), "%s", key);
	if (getenv("EVERLIVE_OSD_DEBUG"))
		fprintf(stderr, "[OSD] details '%s' %dx%d\n",
			card->details_text, card->details_width,
			card->details_height);
	return true;
}

void everlive_osd_render_details(SDL_Renderer *renderer,
				 const struct everlive_osd_card *card,
				 const SDL_Rect *anchor,
				 int viewport_w, int viewport_h)
{
	SDL_Rect dst;
	SDL_Rect scaled;
	int gap;
	int margin;
	int out_w = 0;
	int out_h = 0;
	float s;

	if (!renderer || !card || !card->details || !anchor)
		return;
	if (viewport_w <= 0 || viewport_h <= 0)
		return;
	s = card->details_scale > 0.0f ? card->details_scale : 1.0f;
	gap = iscale(12.0f * s);
	margin = iscale(10.0f * s);
	dst.w = card->details_width;
	dst.h = card->details_height;
	dst.x = anchor->x;
	dst.y = anchor->y + anchor->h + gap;
	if (dst.x < margin)
		dst.x = margin;
	if (dst.x + dst.w > viewport_w - margin)
		dst.x = viewport_w - dst.w - margin;
	if (dst.y + dst.h > viewport_h - margin) {
		dst.y = anchor->y - dst.h - gap;
		if (dst.y < margin)
			dst.y = margin;
	}

	scaled = dst;
	if (SDL_GetRendererOutputSize(renderer, &out_w, &out_h) == 0 &&
	    out_w > 0 && out_h > 0) {
		scaled.x = lroundf((float)dst.x * (float)out_w /
				   (float)viewport_w);
		scaled.y = lroundf((float)dst.y * (float)out_h /
				   (float)viewport_h);
		scaled.w = lroundf((float)dst.w * (float)out_w /
				   (float)viewport_w);
		scaled.h = lroundf((float)dst.h * (float)out_h /
				   (float)viewport_h);
	}
	SDL_RenderCopy(renderer, card->details, NULL, &scaled);
}

bool everlive_osd_build_popup(SDL_Renderer *renderer,
			      enum everlive_osd_popup_kind kind,
			      int selected_input, int hover_input,
			      int volume_pct, int min_width, bool above,
			      float scale, struct everlive_osd_popup *out)
{
	SDL_Surface *surface;
	SDL_Texture *texture;
	float s = clamp_scale(scale);
	const float ss = 4.0f;
	float pu = EVERLIVE_OSD_HUB_SCALE * s * ss;
	int radius = uscale(EVERLIVE_OSD_RADIUS_POPUP, pu);
	int pad = uscale(EVERLIVE_OSD_POPUP_PAD, pu);
	int row_h = uscale(EVERLIVE_OSD_POPUP_ROW_H, pu);
	int label_size = uscale(EVERLIVE_OSD_POPUP_TEXT_SIZE, pu);
	int item_pad = uscale(EVERLIVE_OSD_POPUP_ITEM_PAD_X, pu);
	int border = (int)ss;
	int width;
	int height;
	int i;

	if (!renderer || !out || !g_available || kind == EVERLIVE_OSD_POPUP_NONE)
		return false;
	if (label_size < 11)
		label_size = 11;
	if (border < 1)
		border = 1;

	if (kind == EVERLIVE_OSD_POPUP_ASPECT) {
		static const char *const labels[] = {
			"Auto", "16:9", "4:3", "Stretch"
		};
		int text_w = 0;
		int min_w = min_width > 0 ? min_width * (int)ss : 0;

		for (i = 0; i < 4; i++) {
			int w = text_width(labels[i], g_font_semibold,
					   label_size);

			if (w > text_w)
				text_w = w;
		}
		width = text_w + item_pad * 2;
		if (width < min_w)
			width = min_w;
		height = pad * 2 + row_h * 4;
	} else {
		int min_w = min_width > 0 ? min_width * (int)ss : 0;

		width = uscale(EVERLIVE_OSD_VOLUME_W, pu);
		if (width < min_w)
			width = min_w;
		height = uscale(EVERLIVE_OSD_VOLUME_H, pu);
	}
	if (width < 1 || height < 1)
		return false;

	surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
						 SDL_PIXELFORMAT_RGBA32);
	if (!surface)
		return false;
	SDL_FillRect(surface, NULL, 0);
	if (kind == EVERLIVE_OSD_POPUP_ASPECT) {
		/* Attached combo dropdown: square on the edge touching the chip. */
		int top = above ? radius : 0;
		int bottom = above ? 0 : radius;
		int inner = radius > border ? radius - border : 0;
		int inner_top = above ? inner : 0;
		int inner_bottom = above ? 0 : inner;

		fill_rounded_corners(surface, 0, 0, width, height, top, top,
				     bottom, bottom,
				     surface_color(surface,
						   EVERLIVE_OSD_BLUE_BORDER, 255));
		fill_rounded_corners(surface, border, border,
				     width - border * 2, height - border * 2,
				     inner_top, inner_top, inner_bottom,
				     inner_bottom,
				     surface_color(surface,
						   EVERLIVE_OSD_TOOLTIP_BG, 255));
	} else {
		fill_rounded(surface, 0, 0, width, height, radius,
			     surface_color(surface, EVERLIVE_OSD_BLUE_BORDER,
					   255));
		fill_rounded(surface, border, border, width - border * 2,
			     height - border * 2,
			     radius > border ? radius - border : 0,
			     surface_color(surface, EVERLIVE_OSD_TOOLTIP_BG,
					   255));
	}

	out->kind = kind;
	out->width = (width + (int)ss / 2) / (int)ss;
	out->height = (height + (int)ss / 2) / (int)ss;
	if (out->width < 1)
		out->width = 1;
	if (out->height < 1)
		out->height = 1;
	if (kind == EVERLIVE_OSD_POPUP_ASPECT) {
		static const char *const labels[] = {
			"Auto", "16:9", "4:3", "Stretch"
		};

		for (i = 0; i < 4; i++) {
			SDL_Rect row = { 0, pad + i * row_h, width, row_h };
			bool selected = (i == selected_input);
			bool hovered = (i == hover_input);

			if (selected || hovered) {
				SDL_FillRect(surface, &row,
					     surface_color(surface,
							   EVERLIVE_OSD_BLUE,
							   255));
			}
			draw_text(surface, labels[i], g_font_semibold,
				  label_size, row.x + item_pad,
				  row.y + (row_h - text_height(g_font_semibold,
							       label_size)) / 2,
				  (selected || hovered) ?
					  EVERLIVE_OSD_BLUE_TEXT :
					  EVERLIVE_OSD_TITLE, 255, NULL);
			out->aspect_rows[i].x = row.x / (int)ss;
			out->aspect_rows[i].y = row.y / (int)ss;
			out->aspect_rows[i].w = (row.w + (int)ss / 2) / (int)ss;
			out->aspect_rows[i].h = (row.h + (int)ss / 2) / (int)ss;
		}
	} else {
		int pad_x = uscale(12.0f, pu);
		int icon_size = uscale(EVERLIVE_OSD_ICON_SIZE, pu);
		int gap = uscale(EVERLIVE_OSD_GAP, pu);
		int label_size_v = uscale(EVERLIVE_OSD_LABEL_SIZE, pu);
		int label_w;
		int track_h = uscale(EVERLIVE_OSD_TRACK_H, pu);
		int knob = uscale(EVERLIVE_OSD_KNOB_SIZE, pu);
		int track_x = pad_x + icon_size + gap;
		int track_w;
		int track_y = (height - track_h) / 2;
		int fill_w;
		int pct = volume_pct < 0 ? 0 : (volume_pct > 150 ? 150 : volume_pct);
		char pct_text[16];
		int pct_w;

		if (label_size_v < 11)
			label_size_v = 11;
		label_w = text_width("150%", g_font_mono, label_size_v);
		track_w = width - pad_x * 2 - icon_size - gap * 2 - label_w;
		if (track_w < 1)
			track_w = 1;
		fill_w = (track_w * pct) / 150;
		draw_glyph(surface, EVERLIVE_OSD_ICON_VOLUME, icon_size,
			   pad_x, (height - icon_size) / 2,
			   EVERLIVE_OSD_TEXT_MUTED, 255);
		fill_rounded(surface, track_x, track_y, track_w, track_h,
			     track_h / 2,
			     surface_color(surface, EVERLIVE_OSD_TRACK, 255));
		if (fill_w > 0) {
			fill_rounded(surface, track_x, track_y, fill_w,
				     track_h, track_h / 2,
				     surface_color(surface, EVERLIVE_OSD_BLUE,
						   255));
		}
		fill_circle(surface, track_x + fill_w,
			    track_y + track_h / 2, knob / 2,
			    surface_color(surface, EVERLIVE_OSD_KNOB, 255));
		snprintf(pct_text, sizeof(pct_text), "%d%%", pct);
		pct_w = text_width(pct_text, g_font_mono, label_size_v);
		draw_text(surface, pct_text, g_font_mono, label_size_v,
			  width - pad_x - pct_w,
			  (height - text_height(g_font_mono,
						label_size_v)) / 2,
			  EVERLIVE_OSD_TITLE, 255, NULL);
		out->volume_track.x = track_x / (int)ss;
		out->volume_track.y = track_y / (int)ss;
		out->volume_track.w = (track_w + (int)ss / 2) / (int)ss;
		out->volume_track.h = (track_h + (int)ss / 2) / (int)ss;
	}

	texture = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_FreeSurface(surface);
	if (!texture)
		return false;
	SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND);
	SDL_SetTextureScaleMode(texture, SDL_ScaleModeLinear);
	if (out->texture)
		SDL_DestroyTexture(out->texture);
	out->texture = texture;
	return true;
}

bool everlive_osd_build_text(SDL_Renderer *renderer, const char *text,
			     int viewport_h, SDL_Texture **texture,
			     int *width, int *height, int *scale_out)
{
	char copy[1024];
	char *lines[24];
	int line_count = 0;
	int max_w = 0;
	int line_h;
	int gap;
	int pad;
	int font_size;
	float s;
	SDL_Surface *surface;
	SDL_Texture *built;
	char *p;
	int y;

	if (!renderer || !texture || !g_available || !text)
		return false;
	s = clamp_scale((float)viewport_h / 1080.0f);
	font_size = iscale(16.0f * s);
	if (font_size < 11)
		font_size = 11;
	line_h = text_height(g_font_regular, font_size);
	gap = iscale(5.0f * s);
	pad = iscale(10.0f * s);

	snprintf(copy, sizeof(copy), "%s", text);
	lines[line_count++] = copy;
	for (p = copy; *p && line_count < (int)SDL_arraysize(lines); p++) {
		if (*p == '\n') {
			*p = '\0';
			lines[line_count++] = p + 1;
		}
	}
	for (int i = 0; i < line_count; i++) {
		int w = text_width(lines[i], g_font_regular, font_size);
		if (w > max_w)
			max_w = w;
	}
	if (max_w <= 0)
		return false;
	int w = max_w + pad * 2;
	int h = pad * 2 + line_count * line_h +
		(line_count - 1) * gap;

	surface = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32,
						 SDL_PIXELFORMAT_RGBA32);
	if (!surface)
		return false;
	SDL_FillRect(surface, NULL, 0);
	fill_rounded(surface, 0, 0, w, h, iscale(8.0f * s),
		     surface_color(surface, EVERLIVE_OSD_GROUP_BORDER, 255));
	fill_rounded(surface, 1, 1, w - 2, h - 2,
		     iscale(8.0f * s) > 1 ? iscale(8.0f * s) - 1 : 0,
		     surface_color(surface, EVERLIVE_OSD_TOOLTIP_BG, 235));
	y = pad;
	for (int i = 0; i < line_count; i++) {
		draw_text(surface, lines[i], g_font_regular, font_size, pad, y,
			  EVERLIVE_OSD_TITLE, 255, NULL);
		y += line_h + gap;
	}
	built = SDL_CreateTextureFromSurface(renderer, surface);
	SDL_FreeSurface(surface);
	if (!built)
		return false;
	SDL_SetTextureBlendMode(built, SDL_BLENDMODE_BLEND);
	if (*texture)
		SDL_DestroyTexture(*texture);
	*texture = built;
	if (width)
		*width = w;
	if (height)
		*height = h;
	if (scale_out)
		*scale_out = (int)(s * 2.0f + 0.5f);
	return true;
}
