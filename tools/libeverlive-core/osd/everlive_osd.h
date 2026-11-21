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

#include "everlive_osd_theme.h"

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
#define EVERLIVE_OSD_ICON_DOT		0xF111
#define EVERLIVE_OSD_ICON_HIDE		0xF070 /* eye-slash */
#define EVERLIVE_OSD_ICON_CHART		0xF201 /* chart-line */
#define EVERLIVE_OSD_ICON_INPUT		0xF2F6 /* right-to-bracket */
#define EVERLIVE_OSD_ICON_TABLE_CELLS	0xF00A /* table-cells */
#define EVERLIVE_OSD_ICON_MAXIMIZE	0xF31E
#define EVERLIVE_OSD_ICON_MINIMIZE	0xF78C

enum everlive_osd_action {
	EVERLIVE_OSD_ACTION_NONE = 0,
	EVERLIVE_OSD_ACTION_INPUT_1,
	EVERLIVE_OSD_ACTION_INPUT_2,
	EVERLIVE_OSD_ACTION_ASPECT,
	EVERLIVE_OSD_ACTION_SCALE,
	EVERLIVE_OSD_ACTION_PIP,
	EVERLIVE_OSD_ACTION_VOLUME,
	EVERLIVE_OSD_ACTION_MUTE,
	EVERLIVE_OSD_ACTION_FULLSCREEN,
	EVERLIVE_OSD_ACTION_RECORD,
	EVERLIVE_OSD_ACTION_STATS,
	EVERLIVE_OSD_ACTION_HIDE,
	EVERLIVE_OSD_ACTION_INFO,
	EVERLIVE_OSD_ACTION_COUNT,
};

struct everlive_osd_snapshot {
	const char *title;
	const char *details;		/* identity hover pill */
	bool signal_live;
	bool show_input;
	int input_index;		/* 0/1 on DUO, -1 otherwise */
	int aspect_index;		/* auto/16:9/4:3/stretch */
	bool scale_integer;
	bool pip;
	bool muted;
	bool fullscreen;
	bool recording;
	bool stats_active;
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
	char tooltip_key[96];
	SDL_Texture *details;
	int details_width;
	int details_height;
	float details_scale;
	char details_text[192];
	char details_key[224];
	int input_index;
	int aspect_index;
	bool scale_integer;
	struct everlive_osd_hitbox hits[EVERLIVE_OSD_ACTION_COUNT];
	int hit_count;
};

enum everlive_osd_tone {
	EVERLIVE_OSD_TONE_NEUTRAL = 0,
	EVERLIVE_OSD_TONE_MINT,
	EVERLIVE_OSD_TONE_WARN,
	EVERLIVE_OSD_TONE_DANGER,
};

enum everlive_osd_popup_kind {
	EVERLIVE_OSD_POPUP_NONE = 0,
	EVERLIVE_OSD_POPUP_ASPECT,
	EVERLIVE_OSD_POPUP_VOLUME,
};

struct everlive_osd_popup {
	SDL_Texture *texture;
	int width;
	int height;
	enum everlive_osd_popup_kind kind;
	SDL_Rect aspect_rows[4];
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
			      int selected_input, int hover_input,
			      int volume_pct, int min_width, bool above,
			      float scale, struct everlive_osd_popup *out);
bool everlive_osd_build_tooltip(SDL_Renderer *renderer,
				struct everlive_osd_card *card,
				enum everlive_osd_action action,
				float scale);
void everlive_osd_render_tooltip(SDL_Renderer *renderer,
				 const struct everlive_osd_card *card,
				 const SDL_Rect *anchor,
				 int viewport_w, int viewport_h);
bool everlive_osd_build_details(SDL_Renderer *renderer,
				struct everlive_osd_card *card,
				float scale);
void everlive_osd_render_details(SDL_Renderer *renderer,
				 const struct everlive_osd_card *card,
				 const SDL_Rect *anchor,
				 int viewport_w, int viewport_h);
bool everlive_osd_build_text(SDL_Renderer *renderer, const char *text,
			     int viewport_h, SDL_Texture **texture,
			     int *width, int *height, int *scale_out);
bool everlive_osd_build_toast(SDL_Renderer *renderer, const char *text,
			      enum everlive_osd_tone tone, int viewport_h,
			      SDL_Texture **texture, int *width, int *height,
			      int *scale_out);
void everlive_osd_destroy_card(struct everlive_osd_card *card);
void everlive_osd_destroy_popup(struct everlive_osd_popup *popup);

#endif /* EVERLIVE_OSD_H */
