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
#define OSD_FONT_ICONS "fa-solid-900.ttf"

static TTF_Font *g_font_regular;
static TTF_Font *g_font_semibold;
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

static uint32_t mix_rgb(uint32_t base, uint32_t tint, int percent)
{
	int r = (int)((base >> 16) & 0xff);
	int g = (int)((base >> 8) & 0xff);
	int b = (int)(base & 0xff);
	int tr = (int)((tint >> 16) & 0xff);
	int tg = (int)((tint >> 8) & 0xff);
	int tb = (int)(tint & 0xff);

	r += (tr - r) * percent / 100;
	g += (tg - g) * percent / 100;
	b += (tb - b) * percent / 100;
	return (uint32_t)((r << 16) | (g << 8) | b);
}

static void draw_chip_hover(SDL_Surface *surface, int x, int y, int w, int h,
			    int radius, uint32_t fill, uint32_t border,
			    bool hovered)
{
	uint32_t hot_fill = hovered ? mix_rgb(fill, 0xFFFFFF, 12) : fill;

	draw_chip(surface, x, y, w, h, radius, hot_fill,
		  hovered ? EVERLIVE_OSD_HOVER_BORDER : border);
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
	case EVERLIVE_OSD_ACTION_HIDE:
		return "Hide overlay";
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
	case EVERLIVE_OSD_ACTION_HIDE:
		return "Tab / O";
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

	TTF_SetFontHinting(g_font_regular, TTF_HINTING_LIGHT);
	TTF_SetFontHinting(g_font_semibold, TTF_HINTING_LIGHT);
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
	if (g_font_icons)
		TTF_CloseFont(g_font_icons);
	g_font_regular = NULL;
	g_font_semibold = NULL;
	g_font_icons = NULL;
	g_available = false;
	if (g_initialised)
		TTF_Quit();
	g_initialised = false;
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
	Uint32 border_color;
	Uint32 divider_color;
	int title_size;
	int chip_size;
	int chip_icon_size;
	int pad_x;
	int pad_y;
	int gap;
	int chip_h;
	int chip_pad;
	int chip_gap;
	int button;
	int radius;
	int dot;
	int title_h;
	int title_w;
	int input_seg_w = 0;
	int input_group_w = 0;
	int input_divider;
	int aspect_w;
	int scale_w;
	int divider_w;
	int info_gap;
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
	float s = clamp_scale(scale);
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

	snprintf(out->details_text, sizeof(out->details_text), "%s",
		 snap && snap->details ? snap->details : "");
	out->input_index = input_index;
	out->aspect_index = aspect_index;
	out->scale_integer = scale_integer;

	hover = snap ? (int)snap->hover_action : 0;
	if (hover == (int)EVERLIVE_OSD_ACTION_INFO)
		hover = (int)EVERLIVE_OSD_ACTION_NONE;

	snprintf(key, sizeof(key),
		 "%s|%d|%d|%d|%d|%d|%d|%d|%d|%d|%.3f|%dx%d",
		 title, input_index, aspect_index, scale_integer ? 1 : 0,
		 show_input, snap && snap->pip, snap && snap->muted,
		 snap && snap->fullscreen, snap && snap->recording,
		 hover, s, viewport_w, viewport_h);
	if (out->texture && out->scale == s && out->viewport_w == viewport_w &&
	    out->viewport_h == viewport_h && strcmp(out->cache_key, key) == 0)
		return true;

	out->hit_count = 0;

	title_size = iscale(32.0f * s);
	chip_size = iscale(19.0f * s);
	chip_icon_size = iscale(18.0f * s);
	pad_x = iscale(34.0f * s);
	pad_y = iscale(15.0f * s);
	gap = iscale(14.0f * s);
	chip_h = iscale(48.0f * s);
	chip_pad = iscale(14.0f * s);
	chip_gap = iscale(8.0f * s);
	button = iscale(48.0f * s);
	radius = iscale(12.0f * s);
	dot = iscale(9.0f * s);
	if (title_size < 14)
		title_size = 14;
	if (chip_size < 11)
		chip_size = 11;
	if (chip_icon_size < 12)
		chip_icon_size = 12;
	if (pad_x < 12)
		pad_x = 12;
	if (pad_y < 8)
		pad_y = 8;
	if (gap < 6)
		gap = 6;
	if (chip_h < 24)
		chip_h = 24;
	if (chip_pad < 6)
		chip_pad = 6;
	if (chip_gap < 4)
		chip_gap = 4;
	if (button < 24)
		button = 24;
	if (radius < 5)
		radius = 5;
	if (dot < 4)
		dot = 4;

	title_h = text_height(g_font_semibold, title_size);
	title_w = text_width(title, g_font_semibold, title_size);
	if (show_input) {
		int icon_w = glyph_width(EVERLIVE_OSD_ICON_INPUT, chip_icon_size);
		int label_w = text_width("HDMI 1", g_font_regular, chip_size);

		input_seg_w = chip_pad * 2 + icon_w + chip_gap + label_w;
		input_divider = iscale(1.0f * s);
		if (input_divider < 1)
			input_divider = 1;
		input_group_w = input_seg_w * 2 + input_divider;
	}
	{
		int icon_w = glyph_width(EVERLIVE_OSD_ICON_EXPAND,
					 chip_icon_size);
		int chevron_w = glyph_width(EVERLIVE_OSD_ICON_CHEVRON,
					    iscale(12.0f * s));
		int label_w = text_width("Stretch", g_font_regular, chip_size);

		aspect_w = chip_pad * 2 + icon_w + chip_gap + label_w +
			   chip_gap + chevron_w;
	}
	{
		int icon_w = glyph_width(EVERLIVE_OSD_ICON_TABLE_CELLS,
					 chip_icon_size);
		int label_w = text_width("Bilinear", g_font_regular, chip_size);

		scale_w = chip_pad * 2 + icon_w + chip_gap + label_w;
	}
	divider_w = iscale(1.0f * s);
	if (divider_w < 1)
		divider_w = 1;
	info_gap = iscale(24.0f * s);
	icon_count = 5 + (show_input ? 1 : 0);
	icons_w = icon_count * button + (icon_count - 1) * iscale(9.0f * s);

	info_w = dot + gap + title_w;
	content_w = info_w + info_gap +
		    (show_input ? input_group_w + gap : 0) +
		    aspect_w + gap +
		    scale_w + gap + gap +
		    divider_w + gap +
		    icons_w;
	width = pad_x * 2 + content_w;
	max_width = viewport_w - iscale(20.0f * s) * 2;
	if (max_width < 1)
		max_width = viewport_w;
	if (width > max_width)
		width = max_width;
	if (width < 1)
		return false;
	height = pad_y * 2 + chip_h;

	surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
						 SDL_PIXELFORMAT_RGBA32);
	if (!surface)
		return false;

	SDL_FillRect(surface, NULL, 0);
	bg_color = surface_color(surface, EVERLIVE_OSD_CARD_BG,
				 EVERLIVE_OSD_CARD_ALPHA);
	border_color = surface_color(surface, EVERLIVE_OSD_BORDER, 255);
	divider_color = surface_color(surface, EVERLIVE_OSD_DIVIDER, 255);

	fill_rounded(surface, 0, 0, width, height, radius, border_color);
	fill_rounded(surface, 1, 1, width - 2, height - 2,
		     radius > 1 ? radius - 1 : 0, bg_color);

	title_x = pad_x + dot + gap;
	title_y = pad_y + (chip_h - title_h) / 2;
	chip_y = pad_y;
	{
		int cx = pad_x + dot / 2;
		int cy = pad_y + chip_h / 2;
		int glow_size = iscale(20.0f * s);
		int core_size = iscale(11.0f * s);
		int glow_w;
		int core_w;

		if (glow_size < 8)
			glow_size = 8;
		if (core_size < 6)
			core_size = 6;
		glow_w = glyph_width(EVERLIVE_OSD_ICON_DOT, glow_size);
		core_w = glyph_width(EVERLIVE_OSD_ICON_DOT, core_size);
		draw_glyph(surface, EVERLIVE_OSD_ICON_DOT, glow_size,
			   cx - glow_w / 2, cy - glow_size / 2,
			   EVERLIVE_OSD_LIVE, 60);
		draw_glyph(surface, EVERLIVE_OSD_ICON_DOT, core_size,
			   cx - core_w / 2, cy - core_size / 2,
			   EVERLIVE_OSD_LIVE, 255);
	}
	draw_text(surface, title, g_font_semibold, title_size,
		  title_x, title_y, EVERLIVE_OSD_TITLE, 255, NULL);
	add_hit(out, (SDL_Rect){ pad_x, chip_y, info_w, chip_h },
		EVERLIVE_OSD_ACTION_INFO);

	x = pad_x + info_w + info_gap;
	if (show_input) {
		bool group_hot =
			snapshot_hover(snap, EVERLIVE_OSD_ACTION_INPUT_1) ||
			snapshot_hover(snap, EVERLIVE_OSD_ACTION_INPUT_2);
		int icon_w = glyph_width(EVERLIVE_OSD_ICON_INPUT,
					 chip_icon_size);
		int i;

		draw_chip_hover(surface, x, chip_y, input_group_w, chip_h,
				radius, EVERLIVE_OSD_CHIP_BG,
				EVERLIVE_OSD_BORDER, group_hot);
		for (i = 0; i < 2; i++) {
			bool active = i == input_index;
			bool hot = snapshot_hover(snap,
						  i == 0 ?
						  EVERLIVE_OSD_ACTION_INPUT_1 :
						  EVERLIVE_OSD_ACTION_INPUT_2);
			uint32_t fill = active ? EVERLIVE_OSD_INPUT_BG :
						 EVERLIVE_OSD_CHIP_BG;
			uint32_t color = active ? EVERLIVE_OSD_TITLE :
						  EVERLIVE_OSD_ROUTE;
			const char *label = i == 0 ? "HDMI 1" : "HDMI 2";
			int label_w = text_width(label, g_font_regular,
						 chip_size);
			int seg_x = x + i * (input_seg_w + input_divider);

			if (active || hot) {
				if (hot)
					fill = mix_rgb(fill, 0xFFFFFF, 12);
				fill_rounded(surface, seg_x + 1, chip_y + 1,
					     input_seg_w - 2, chip_h - 2,
					     radius > 1 ? radius - 1 : 0,
					     surface_color(surface, fill, 255));
			}
			text_y = chip_y + (chip_h -
					   text_height(g_font_regular,
						       chip_size)) / 2;
			if (active) {
				icon_x = seg_x + chip_pad;
				draw_glyph(surface, EVERLIVE_OSD_ICON_INPUT,
					   chip_icon_size, icon_x,
					   chip_y + (chip_h -
						     chip_icon_size) / 2,
					   color, 255);
				text_x = icon_x + icon_w + chip_gap;
			} else {
				text_x = seg_x + (input_seg_w - label_w) / 2;
			}
			draw_text(surface, label, g_font_regular, chip_size,
				  text_x, text_y, color, 255, NULL);
			add_hit(out, (SDL_Rect){ seg_x, chip_y, input_seg_w,
						 chip_h },
				i == 0 ? EVERLIVE_OSD_ACTION_INPUT_1 :
					 EVERLIVE_OSD_ACTION_INPUT_2);
		}
		SDL_FillRect(surface,
			     &(SDL_Rect){ x + input_seg_w,
					  chip_y + iscale(6.0f * s), input_divider,
					  chip_h - iscale(12.0f * s) },
			     divider_color);
		x += input_group_w + gap;
	}

	{
		static const char *const names[] = {
			"Auto", "16:9", "4:3", "Stretch"
		};
		bool hot = snapshot_hover(snap, EVERLIVE_OSD_ACTION_ASPECT);
		int icon_w = glyph_width(EVERLIVE_OSD_ICON_EXPAND,
					 chip_icon_size);
		int chevron_w = glyph_width(EVERLIVE_OSD_ICON_CHEVRON,
					    iscale(12.0f * s));

		draw_chip_hover(surface, x, chip_y, aspect_w, chip_h, radius,
				EVERLIVE_OSD_CHIP_BG, EVERLIVE_OSD_BORDER,
				hot);
		icon_x = x + chip_pad;
		draw_glyph(surface, EVERLIVE_OSD_ICON_EXPAND, chip_icon_size,
			   icon_x, chip_y + (chip_h - chip_icon_size) / 2,
			   EVERLIVE_OSD_TITLE, 255);
		text_x = icon_x + icon_w + chip_gap;
		text_y = chip_y + (chip_h - text_height(g_font_regular,
						       chip_size)) / 2;
		draw_text(surface, names[aspect_index], g_font_regular,
			  chip_size, text_x, text_y, EVERLIVE_OSD_TITLE, 255,
			  NULL);
		draw_glyph(surface, EVERLIVE_OSD_ICON_CHEVRON,
			   iscale(12.0f * s),
			   x + aspect_w - chip_pad - chevron_w,
			   chip_y + (chip_h - iscale(12.0f * s)) / 2,
			   EVERLIVE_OSD_ROUTE, 255);
		add_hit(out, (SDL_Rect){ x, chip_y, aspect_w, chip_h },
			EVERLIVE_OSD_ACTION_ASPECT);
		x += aspect_w + gap;
	}

	{
		bool hot = snapshot_hover(snap, EVERLIVE_OSD_ACTION_SCALE);
		int icon_w = glyph_width(EVERLIVE_OSD_ICON_TABLE_CELLS,
					 chip_icon_size);

		draw_chip_hover(surface, x, chip_y, scale_w, chip_h, radius,
				EVERLIVE_OSD_CHIP_BG, EVERLIVE_OSD_BORDER,
				hot);
		icon_x = x + chip_pad;
		draw_glyph(surface, EVERLIVE_OSD_ICON_TABLE_CELLS,
			   chip_icon_size, icon_x,
			   chip_y + (chip_h - chip_icon_size) / 2,
			   EVERLIVE_OSD_TITLE, 255);
		text_x = icon_x + icon_w + chip_gap;
		text_y = chip_y + (chip_h - text_height(g_font_regular,
						       chip_size)) / 2;
		draw_text(surface, scale_integer ? "Integer" : "Bilinear",
			  g_font_regular, chip_size, text_x, text_y,
			  EVERLIVE_OSD_TITLE, 255, NULL);
		add_hit(out, (SDL_Rect){ x, chip_y, scale_w, chip_h },
			EVERLIVE_OSD_ACTION_SCALE);
		x += scale_w + gap + gap;
	}

	{
		SDL_Rect divider = { x, chip_y + iscale(6.0f * s), divider_w,
				     chip_h - iscale(12.0f * s) };

		SDL_FillRect(surface, &divider, divider_color);
	}
	x += divider_w + gap;

#define DRAW_ICON_BUTTON(action, glyph, fill, border, color)		\
	do {								\
		SDL_Rect hit = { x, chip_y, button, button };		\
		int glyph_w = glyph_width((glyph), iscale(24.0f * s));	\
		draw_chip_hover(surface, x, chip_y, button, button, radius, \
				(fill), (border),			\
				snapshot_hover(snap, (action)));	\
		draw_glyph(surface, (glyph), iscale(24.0f * s),		\
			   x + (button - glyph_w) / 2,			\
			   chip_y + (button - iscale(24.0f * s)) / 2,	\
			   (color), 255);				\
		add_hit(out, hit, (action));				\
		x += button + iscale(9.0f * s);				\
	} while (0)

	DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_RECORD,
			 snap && snap->recording ? EVERLIVE_OSD_ICON_STOP : EVERLIVE_OSD_ICON_RECORD,
			 snap && snap->recording ? EVERLIVE_OSD_DANGER_BG : EVERLIVE_OSD_CHIP_BG,
			 snap && snap->recording ? EVERLIVE_OSD_DANGER_BORDER : EVERLIVE_OSD_BORDER,
			 snap && snap->recording ? EVERLIVE_OSD_DANGER : EVERLIVE_OSD_TITLE);
	if (show_input) {
		DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_PIP,
				 EVERLIVE_OSD_ICON_PIP,
				 snap && snap->pip ? EVERLIVE_OSD_BLUE : EVERLIVE_OSD_CHIP_BG,
				 snap && snap->pip ? EVERLIVE_OSD_BLUE_BORDER : EVERLIVE_OSD_BORDER,
				 snap && snap->pip ? EVERLIVE_OSD_BLUE_TEXT : EVERLIVE_OSD_TITLE);
	}
	DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_VOLUME,
			 EVERLIVE_OSD_ICON_VOLUME,
			 EVERLIVE_OSD_CHIP_BG, EVERLIVE_OSD_BORDER,
			 EVERLIVE_OSD_TITLE);
	DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_MUTE,
			 EVERLIVE_OSD_ICON_MUTE,
			 snap && snap->muted ? EVERLIVE_OSD_DANGER_BG : EVERLIVE_OSD_CHIP_BG,
			 snap && snap->muted ? EVERLIVE_OSD_DANGER_BORDER : EVERLIVE_OSD_BORDER,
			 snap && snap->muted ? EVERLIVE_OSD_DANGER : EVERLIVE_OSD_TITLE);
	DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_FULLSCREEN,
			 snap && snap->fullscreen ? EVERLIVE_OSD_ICON_MINIMIZE : EVERLIVE_OSD_ICON_MAXIMIZE,
			 snap && snap->fullscreen ? EVERLIVE_OSD_DANGER_BG : EVERLIVE_OSD_CHIP_BG,
			 snap && snap->fullscreen ? EVERLIVE_OSD_DANGER_BORDER : EVERLIVE_OSD_BORDER,
			 snap && snap->fullscreen ? EVERLIVE_OSD_DANGER : EVERLIVE_OSD_TITLE);
	DRAW_ICON_BUTTON(EVERLIVE_OSD_ACTION_HIDE,
			 EVERLIVE_OSD_ICON_HIDE,
			 EVERLIVE_OSD_CHIP_BG, EVERLIVE_OSD_BORDER,
			 EVERLIVE_OSD_TITLE);
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
			"[OSD] bar gen=%u scale=%.2f viewport=%dx%d size=%dx%d hits=%d\n",
			out->generation, s, viewport_w, viewport_h, width,
			height, out->hit_count);
	return true;
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

	label_size = iscale(15.0f * s);
	key_size = iscale(14.0f * s);
	if (label_size < 11)
		label_size = 11;
	if (key_size < 10)
		key_size = 10;
	pad = iscale(10.0f * s);
	gap = iscale(12.0f * s);
	key_pad = iscale(7.0f * s);
	radius = iscale(7.0f * s);
	label_w = text_width(label, g_font_regular, label_size);
	label_h = text_height(g_font_regular, label_size);
	if (detail) {
		detail_w = text_width(detail, g_font_regular, label_size);
		detail_h = text_height(g_font_regular, label_size);
	}
	key_w = text_width(shortcut, g_font_semibold, key_size);
	key_h = text_height(g_font_semibold, key_size);
	cap_h = key_h + iscale(4.0f * s);
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
		     surface_color(surface, EVERLIVE_OSD_HOVER_BORDER, 255));
	fill_rounded(surface, 1, 1, width - 2, height - 2,
		     radius > 1 ? radius - 1 : 0,
		     surface_color(surface, EVERLIVE_OSD_TOOLTIP_BG, 242));
	label_y = (height - label_h) / 2;
	draw_text(surface, label, g_font_regular, label_size, pad, label_y,
		  EVERLIVE_OSD_TITLE, 255, NULL);
	detail_x = pad + label_w + gap;
	if (detail) {
		detail_y = (height - detail_h) / 2;
		draw_text(surface, detail, g_font_regular, label_size,
			  detail_x, detail_y, EVERLIVE_OSD_ROUTE, 255, NULL);
	}
	cap_x = detail ? detail_x + detail_w + gap : pad + label_w + gap;
	cap_y = (height - cap_h) / 2;
	draw_chip(surface, cap_x, cap_y, cap_w, cap_h, radius / 2,
		  EVERLIVE_OSD_CHIP_BG, EVERLIVE_OSD_BORDER);
	draw_text(surface, shortcut, g_font_semibold, key_size,
		  cap_x + key_pad, cap_y + (cap_h - key_h) / 2,
		  EVERLIVE_OSD_MINT, 255, NULL);

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
	float ps = s * ss;
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

	font_size = iscale(18.0f * ps);
	if (font_size < 12)
		font_size = 12;
	pad_x = iscale(14.0f * ps);
	pad_y = iscale(10.0f * ps);
	radius = iscale(10.0f * ps);
	border = (int)ss;
	if (border < 1)
		border = 1;
	text_h = text_height(g_font_regular, font_size);
	text_w = text_width(card->details_text, g_font_regular, font_size);
	width = pad_x * 2 + text_w;
	height = pad_y * 2 + text_h;
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
		     surface_color(surface, EVERLIVE_OSD_TOOLTIP_BG, 242));
	draw_text(surface, card->details_text, g_font_regular, font_size,
		  pad_x, pad_y, EVERLIVE_OSD_ROUTE, 255, NULL);

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
			      int volume_pct, float scale,
			      struct everlive_osd_popup *out)
{
	SDL_Surface *surface;
	SDL_Texture *texture;
	float s = clamp_scale(scale);
	const float ss = 4.0f;
	float ps = s * ss;
	int radius = iscale(8.0f * ps);
	int pad = iscale(10.0f * ps);
	int row_h = iscale(38.0f * ps);
	int label_size = iscale(17.0f * ps);
	int width;
	int height;
	int i;

	if (!renderer || !out || !g_available || kind == EVERLIVE_OSD_POPUP_NONE)
		return false;

	if (kind == EVERLIVE_OSD_POPUP_ASPECT) {
		static const char *const labels[] = {
			"Auto", "16:9", "4:3", "Stretch"
		};
		int text_w = 0;

		for (i = 0; i < 4; i++) {
			int w = text_width(labels[i], g_font_regular,
					   label_size);

			if (w > text_w)
				text_w = w;
		}
		width = text_w + pad * 2 + iscale(40.0f * ps);
		if (width < iscale(116.0f * ps))
			width = iscale(116.0f * ps);
		height = pad * 2 + row_h * 4;
	} else {
		width = iscale(250.0f * ps);
		height = iscale(62.0f * ps);
	}
	if (width < 1 || height < 1)
		return false;

	surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32,
						 SDL_PIXELFORMAT_RGBA32);
	if (!surface)
		return false;
	SDL_FillRect(surface, NULL, 0);
	fill_rounded(surface, 0, 0, width, height, radius,
		     surface_color(surface, EVERLIVE_OSD_BLUE_BORDER, 255));
	{
		int border = (int)ss;

		if (border < 1)
			border = 1;
		fill_rounded(surface, border, border, width - border * 2,
			     height - border * 2,
			     radius > border ? radius - border : 0,
			     surface_color(surface, EVERLIVE_OSD_CHIP_BG, 255));
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
			SDL_Rect row = { pad, pad + i * row_h, width - pad * 2,
					 row_h };
			bool selected = (i == selected_input);
			bool hovered = (i == hover_input);

			if (selected || hovered) {
				fill_rounded(surface, row.x, row.y, row.w, row.h,
					     radius / 2,
					     surface_color(surface,
							   EVERLIVE_OSD_BLUE,
							   255));
			}
			draw_text(surface, labels[i], g_font_regular, label_size,
				  row.x + iscale(14.0f * ps),
				  row.y + (row_h - text_height(g_font_regular,
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
		SDL_Rect track = { pad + iscale(8.0f * ps),
				   height / 2 - iscale(4.0f * ps),
				   width - pad * 2 - iscale(16.0f * ps),
				   iscale(7.0f * ps) };
		int fill_w = (track.w * (volume_pct < 0 ? 0 :
					 (volume_pct > 150 ? 150 : volume_pct))) / 150;

		fill_rounded(surface, track.x, track.y, track.w, track.h,
			     track.h / 2,
			     surface_color(surface, EVERLIVE_OSD_DIVIDER, 255));
		if (fill_w > 0) {
			fill_rounded(surface, track.x, track.y, fill_w,
				     track.h, track.h / 2,
				     surface_color(surface, EVERLIVE_OSD_BLUE,
						   255));
		}
		fill_circle(surface, track.x + fill_w,
			    track.y + track.h / 2, iscale(8.0f * ps),
			    surface_color(surface, EVERLIVE_OSD_TITLE, 255));
		out->volume_track.x = track.x / (int)ss;
		out->volume_track.y = track.y / (int)ss;
		out->volume_track.w = (track.w + (int)ss / 2) / (int)ss;
		out->volume_track.h = (track.h + (int)ss / 2) / (int)ss;
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
		     surface_color(surface, EVERLIVE_OSD_BORDER, 255));
	fill_rounded(surface, 1, 1, w - 2, h - 2,
		     iscale(8.0f * s) > 1 ? iscale(8.0f * s) - 1 : 0,
		     surface_color(surface, 0x161927, 235));
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
