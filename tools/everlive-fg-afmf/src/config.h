#pragma once

#include <stdbool.h>
#include <stdint.h>

enum afmf_performance_mode {
    AFMF_PERFORMANCE_AUTO = 0,    /* half-resolution flow from 2560x1440 upwards */
    AFMF_PERFORMANCE_QUALITY = 1, /* optical flow at display resolution */
    AFMF_PERFORMANCE_FAST = 2,    /* optical flow at half resolution, 16-pixel blocks on screen */
};

enum afmf_search_mode {
    AFMF_SEARCH_AUTO = 0,     /* 7 levels at full flow resolution, 5 at reduced */
    AFMF_SEARCH_STANDARD = 1, /* 5 levels */
    AFMF_SEARCH_HIGH = 2,     /* 7 levels always */
};

enum afmf_fast_motion_response {
    AFMF_RESPONSE_REPEAT_FRAMES = 0, /* untrusted pixels show the previous frame again */
    AFMF_RESPONSE_BLENDED_FRAMES = 1, /* untrusted pixels blend the two frames without motion */
};

enum afmf_present_mode {
    AFMF_PRESENT_AUTO = 0, /* FIFO becomes MAILBOX when the surface offers it */
    AFMF_PRESENT_KEEP = 1, /* the application's present mode, untouched */
};

struct afmf_config {
    int log_level;               /* AFMF_LOG, 0..3; default AFMF_LOG_ERROR */
    uint32_t extra_images;       /* AFMF_EXTRA_IMAGES, 1..8: swapchain images added for generation */
    uint64_t acquire_timeout_ns; /* AFMF_ACQUIRE_TIMEOUT_US: how long to wait for the spare's release */
    bool interpolate;            /* AFMF_INTERPOLATE=0 falls back to repeating the previous frame */
    enum afmf_search_mode search_mode; /* AFMF_SEARCH_MODE: auto | standard | high */
    enum afmf_fast_motion_response fast_motion; /* AFMF_FAST_MOTION_RESPONSE: repeat | blend */
    enum afmf_performance_mode performance;     /* AFMF_PERFORMANCE_MODE: auto | quality | performance */
    const char *dump_dir;        /* AFMF_DUMP_DIR: where the first generated frames are written as PPM */
    bool profile;                /* AFMF_PROFILE=1 (or AFMF_LOG=3): GPU time per pass, logged periodically */
    bool async;                  /* AFMF_ASYNC=0 keeps the work on the application's queue */
    bool pacing;                 /* AFMF_PACING=0 presents the real frame right behind the generated one */
    bool passive;                /* AFMF_GAMESCOPE=1 and this process is Gamescope itself: pass-through */
    enum afmf_present_mode present_mode; /* AFMF_PRESENT_MODE: auto | keep */
    bool governor;               /* AFMF_GOVERNOR=0 keeps generating every frame however late the GPU runs */
    uint32_t static_block_sad;   /* AFMF_STATIC_BLOCK_SAD: a block at rest under this SAD skips the search (0 = never) */
    bool direct_output;          /* AFMF_DIRECT_OUTPUT=1: interpolate straight into the swapchain image (STORAGE usage on it) */
    bool hud_detect;             /* AFMF_HUD_DETECT: a pixel unchanged between the frames on a moving block is kept, not warped */
    bool direct_ingest;          /* AFMF_DIRECT_INGEST: sample the game's frame straight into the ring and luma (SAMPLED usage) */
    bool sad_int16;              /* AFMF_SAD_INT16=0: the block search's scalar SAD even where shaderInt16 is available */
    uint32_t min_fps;            /* AFMF_MIN_FPS: no companions below this real frame rate (0 = always) */
    uint32_t multiplier;         /* AFMF_MULTIPLIER: 2, 3 or 4; default 2 */
    float cas_sharpness;         /* AFMF_CAS_SHARPNESS: 0.0 .. 1.0; default 0.4 */
    float hud_threshold;         /* AFMF_HUD_THRESHOLD: 0.0 .. 1.0; default 0.045 */
    bool invalid;                /* some variable was set but unparsable; caller reports it */
};

/* Parsed from the environment once, on first use; read-only afterwards. */
const struct afmf_config *afmf_config_get(void);
