/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Shared Console Hub OSD renderer.
 *
 * The engine owns the SDL window and the video renderer; this module owns the
 * presentation of the identity/control card.  It deliberately lives outside
 * libeverlive-core so the core library does not grow an SDL/TTF dependency.
 */

#ifndef EVERLIVE_OSD_H
#define EVERLIVE_OSD_H

#include <SDL.h>
#include <stdbool.h>
#include <stdint.h>

/* Palette mirrored in tools/everlive/ui_theme.py. */
#define EVERLIVE_OSD_CARD_BG	0x141724
#define EVERLIVE_OSD_CARD_ALPHA	217	/* ~85% */
#define EVERLIVE_OSD_BORDER	0x3F455C
#define EVERLIVE_OSD_TITLE	0xE7EBF2
#define EVERLIVE_OSD_ROUTE	0x94A3BB
#define EVERLIVE_OSD_INPUT_BG	0x25334A
#define EVERLIVE_OSD_CHIP_BG	0x202334
#define EVERLIVE_OSD_HOVER_BORDER 0x3A3E54
#define EVERLIVE_OSD_TOOLTIP_BG	0x161927
#define EVERLIVE_OSD_DIVIDER	0x334155
#define EVERLIVE_OSD_LIVE	0x19FF3B
#define EVERLIVE_OSD_MINT_BG	0x23423B
#define EVERLIVE_OSD_MINT	0x7FD8C0
#define EVERLIVE_OSD_DANGER_BG	0x3B1D25
#define EVERLIVE_OSD_DANGER_BORDER 0x7A2A38
#define EVERLIVE_OSD_DANGER	0xFF6B7A
#define EVERLIVE_OSD_BLUE	0x3B5B8C
#define EVERLIVE_OSD_BLUE_BORDER 0x4A6FA5
#define EVERLIVE_OSD_BLUE_TEXT	0xE4EEFF

/* Font Awesome Free 6 solid codepoints. */
#define EVERLIVE_OSD_ICON_CHEVRON	0xF078
#define EVERLIVE_OSD_ICON_PIP		0xF2D2 /* window-restore / PiP */
#define EVERLIVE_OSD_ICON_VOLUME		0xF028
#define EVERLIVE_OSD_ICON_MUTE		0xF6A9
#define EVERLIVE_OSD_ICON_EXPAND		0xF065
#define EVERLIVE_OSD_ICON_COMPRESS	0xF066
#define EVERLIVE_OSD_ICON_ARROW_RIGHT	0xF061
#define EVERLIVE_OSD_ICON_RECORD		0xF111 /* solid circle */
#define EVERLIVE_OSD_ICON_STOP		0xF04D

enum everlive_osd_action {
	EVERLIVE_OSD_ACTION_NONE = 0,
	EVERLIVE_OSD_ACTION_INPUT,
	EVERLIVE_OSD_ACTION_ASPECT,
	EVERLIVE_OSD_ACTION_SCALE,
	EVERLIVE_OSD_ACTION_PIP,
	EVERLIVE_OSD_ACTION_VOLUME,
	EVERLIVE_OSD_ACTION_MUTE,
	EVERLIVE_OSD_ACTION_FULLSCREEN,
	EVERLIVE_OSD_ACTION_RECORD,
	EVERLIVE_OSD_ACTION_COUNT,
};

struct everlive_osd_snapshot {
	const char *title;
	const char *route;
	const char *route_source;
	const char *route_target;
	const char *input_label;	/* "HDMI 1" on DUO, NULL on GC573 */
	const char *aspect_label;	/* "Aspect" */
	const char *scale_label;	/* "Scale" */
	bool show_input;
	bool pip;
	bool muted;
	bool fullscreen;
	bool recording;
	bool interactive;
	enum everlive_osd_action hover_action;
	int volume_pct;
};

struct everlive_osd_hitbox {
	SDL_Rect rect;
	enum everlive_osd_action action;
};

struct everlive_osd_card {
	SDL_Texture *texture;
	int width;
	int height;
	float scale;
	int viewport_w;
	int viewport_h;
	unsigned generation;
	char cache_key[768];
	SDL_Texture *tooltip;
	int tooltip_width;
	int tooltip_height;
	enum everlive_osd_action tooltip_action;
	float tooltip_scale;
	struct everlive_osd_hitbox hits[EVERLIVE_OSD_ACTION_COUNT];
	int hit_count;
};

enum everlive_osd_popup_kind {
	EVERLIVE_OSD_POPUP_NONE = 0,
	EVERLIVE_OSD_POPUP_INPUT,
	EVERLIVE_OSD_POPUP_VOLUME,
};

struct everlive_osd_popup {
	SDL_Texture *texture;
	int width;
	int height;
	enum everlive_osd_popup_kind kind;
	SDL_Rect input_rows[2];
	SDL_Rect volume_track;
};

int everlive_osd_init(void);
void everlive_osd_shutdown(void);
bool everlive_osd_available(void);
void everlive_osd_set_font_dir(const char *dir);

bool everlive_osd_build_card(SDL_Renderer *renderer,
			     const struct everlive_osd_snapshot *snap,
			     float scale, int viewport_w, int viewport_h,
			     struct everlive_osd_card *out);
bool everlive_osd_build_popup(SDL_Renderer *renderer,
			      enum everlive_osd_popup_kind kind,
			      int selected_input, int volume_pct,
			      float scale,
			      struct everlive_osd_popup *out);
bool everlive_osd_build_tooltip(SDL_Renderer *renderer,
				struct everlive_osd_card *card,
				enum everlive_osd_action action,
				float scale);
void everlive_osd_render_tooltip(SDL_Renderer *renderer,
				 const struct everlive_osd_card *card,
				 const SDL_Rect *anchor,
				 int viewport_w, int viewport_h);
bool everlive_osd_build_text(SDL_Renderer *renderer, const char *text,
			     int viewport_h, SDL_Texture **texture,
			     int *width, int *height, int *scale_out);
void everlive_osd_destroy_card(struct everlive_osd_card *card);
void everlive_osd_destroy_popup(struct everlive_osd_popup *popup);

#endif /* EVERLIVE_OSD_H */
