// SPDX-License-Identifier: GPL-2.0-only
/*
 * everlive_display.c - Dedicated Zero-Latency Direct V4L2/SDL2 Console Display & Audio Companion
 *
 * Designed specifically for interactive gaming on capture cards.
 * Features:
 * - Direct V4L2 DMA mmap texture upload (< 2 ms display latency)
 * - Built-in low-latency ALSA stereo LPCM capture (PipeWire loopback with drift compensation)
 * - Interactive on-screen HUD: Real-time volume slider, mute, signal & stats overlay
 * - Mouse wheel volume control & on-screen volume toasts
 * - Auto-hiding cursor & HUD on mouse inactivity
 * - Native borderless fullscreen toggle (F11 / f / double-click)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/wait.h>
#include <spawn.h>
#include <linux/videodev2.h>
#include <pthread.h>
#include <time.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <glob.h>
#include <libgen.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#include "font8x8.h"
#include "everlive_audio_probe.h"

#define BUFFER_COUNT 8

static inline uint64_t diff_timespec_us(const struct timespec *end, const struct timespec *start)
{
    int64_t sec = (int64_t)end->tv_sec - (int64_t)start->tv_sec;
    int64_t nsec = (int64_t)end->tv_nsec - (int64_t)start->tv_nsec;
    int64_t total_us = sec * 1000000LL + nsec / 1000LL;
    return (total_us > 0) ? (uint64_t)total_us : 0;
}

static const char *detect_vdev(void)
{
    static char buf[64];
    glob_t g;
    /* 1. Primary: Direct sysfs driver link for open_gc573 */
    if (glob("/sys/bus/pci/drivers/open_gc573/*/video4linux/video*", 0, NULL, &g) == 0 && g.gl_pathc > 0) {
        char *base = strrchr(g.gl_pathv[0], '/');
        if (base) {
            snprintf(buf, sizeof(buf), "/dev/%s", base + 1);
            globfree(&g);
            if (access(buf, F_OK) == 0)
                return buf;
        }
        globfree(&g);
    }
    /* 2. Secondary: Scan V4L2 device names for GC573 / Live Gamer 4K */
    if (glob("/sys/class/video4linux/video*/name", 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            FILE *f = fopen(g.gl_pathv[i], "r");
            if (f) {
                char name[128];
                if (fgets(name, sizeof(name), f)) {
                    if (strstr(name, "Live Gamer 4K") || strstr(name, "GC573") || strstr(name, "open_gc573")) {
                        fclose(f);
                        char *vnode = strstr(g.gl_pathv[i], "video");
                        if (vnode) {
                            char *slash = strchr(vnode, '/');
                            if (slash) *slash = '\0';
                            snprintf(buf, sizeof(buf), "/dev/%s", vnode);
                            globfree(&g);
                            if (access(buf, F_OK) == 0)
                                return buf;
                        }
                        break;
                    }
                }
                fclose(f);
            }
        }
        globfree(&g);
    }
    return NULL;
}

static const char *detect_adev(void)
{
    static char buf[64];
    FILE *f = fopen("/proc/asound/cards", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            if (strstr(line, "GC573") || strstr(line, "Live Gamer 4K") || strstr(line, "A4K")) {
                char *open_br = strchr(line, '[');
                char *close_br = strchr(line, ']');
                if (open_br && close_br && close_br > open_br) {
                    *close_br = '\0';
                    char *id = open_br + 1;
                    while (*id == ' ') id++;
                    char *end = id + strlen(id) - 1;
                    while (end > id && *end == ' ') *end-- = '\0';
                    snprintf(buf, sizeof(buf), "hw:CARD=%s,DEV=0", id);
                    fclose(f);
                    return buf;
                }
            }
        }
        fclose(f);
    }
    return "hw:CARD=A4K,DEV=0";
}

struct v4l2_mmap_buffer {
    void *start;
    size_t length;
};

static volatile sig_atomic_t g_running = 1;
static bool g_fullscreen = false;
static bool g_muted = false;
static float g_volume = 1.0f;
static volatile sig_atomic_t g_audio_dirty = 1;
static bool g_show_stats = true;
static bool g_hud_visible = true;
static bool g_streamer_mode = false;
static uint32_t g_last_mouse_time = 0;
static uint32_t g_volume_toast_time = 0;
static char g_toast_custom[64] = "";
static uint32_t g_toast_time = 0;
static uintptr_t g_embed_wid = 0;
static uint32_t g_preferred_fmt = 0;
static int g_hdr_mode = 0; // 0 = SDR Monitor Tone-Map (ACES), 1 = HDR Monitor Passthrough (Native), 2 = SDR Bypass
static float g_hdr_exposure = 50.0f; // Scale factor for HDR tone mapping

static pid_t g_audio_pid = 0;   /* loopback, or arecord in the ALSA fallback */
static pid_t g_audio_pid2 = 0;  /* pw-play in the ALSA fallback */
static volatile sig_atomic_t g_audio_resync_needed = 0;

/* Validated --pw-node override (node name or numeric object id). */
static char g_pw_override_name[AUDIO_NODE_NAME_MAX];
static uint32_t g_pw_override_id = 0;
static bool g_pw_override_is_id = false;

static void sig_handler(int sig)
{
    (void)sig;
    g_running = 0;
}

static volatile sig_atomic_t g_need_geom_check = 30;

static void sigwinch_handler(int sig)
{
    (void)sig;
    g_need_geom_check = 30;
}

static void sigusr1_handler(int sig)
{
    (void)sig;
    g_show_stats = !g_show_stats;
}

#define AUDIO_CMD_TIMEOUT_MS 2000
#define AUDIO_PROBE_TIMEOUT_MS 3000
#define AUDIO_BACKOFF_MIN_MS 500
#define AUDIO_BACKOFF_MAX_MS 5000

static bool audio_debug_enabled(void)
{
    return getenv("EVERLIVE_AUDIO_DEBUG") != NULL;
}

static int64_t audio_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Reaps @pid without blocking longer than @timeout_ms. */
static bool audio_reap(pid_t pid, int timeout_ms, int *status_out, bool *timed_out)
{
    int64_t deadline = audio_now_ms() + timeout_ms;
    int status = 0;
    pid_t r;

    for (;;) {
        r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (status_out) *status_out = status;
            if (timed_out) *timed_out = false;
            return true;
        }
        if (r < 0 && errno != EINTR) {
            if (status_out) *status_out = 0;
            if (timed_out) *timed_out = false;
            return false;
        }
        if (audio_now_ms() >= deadline)
            break;
        usleep(5000);
    }

    kill(pid, SIGTERM);
    for (int i = 0; i < 10; i++) {
        usleep(20000);
        r = waitpid(pid, &status, WNOHANG);
        if (r == pid) {
            if (status_out) *status_out = status;
            if (timed_out) *timed_out = true;
            return true;
        }
    }

    kill(pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (status_out) *status_out = status;
    if (timed_out) *timed_out = true;
    return true;
}

/* Spawns an argv array (no shell).  stdin_fd/stdout_fd may be -1 to inherit.
 * When @silence is set, streams that are not redirected go to /dev/null. */
static int audio_spawn(char *const argv[], int stdin_fd, int stdout_fd,
                       bool silence, bool new_group, pid_t *pid_out)
{
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    posix_spawnattr_t *attrp = NULL;
    pid_t pid = 0;
    int rc;

    posix_spawn_file_actions_init(&actions);

    if (stdin_fd >= 0)
        posix_spawn_file_actions_adddup2(&actions, stdin_fd, STDIN_FILENO);
    if (stdout_fd >= 0)
        posix_spawn_file_actions_adddup2(&actions, stdout_fd, STDOUT_FILENO);

    if (silence) {
        if (stdout_fd < 0)
            posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
                                             "/dev/null", O_WRONLY, 0);
        if (!audio_debug_enabled())
            posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                             "/dev/null", O_WRONLY, 0);
    }

    if (new_group) {
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);
        attrp = &attr;
    }

    rc = posix_spawnp(&pid, argv[0], &actions, attrp, argv, environ);

    if (new_group)
        posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);

    if (rc != 0)
        return rc;

    *pid_out = pid;
    return 0;
}

/* Runs argv and captures stdout through a pipe.
 * Returns 0 on exit code 0, a positive exit code on tool failure, -1 on spawn
 * error, -2 on timeout and -3 when the output cap is exceeded. */
static int audio_capture(char *const argv[], char **out, size_t *out_len,
                         size_t max_out, int timeout_ms, const char *label)
{
    posix_spawn_file_actions_t actions;
    int fds[2];
    pid_t pid = 0;
    char *buf;
    size_t used = 0;
    bool child_done = false;
    bool eof = false;
    bool overflow = false;
    bool timed_out = false;
    int child_status = 0;
    int rc;
    int64_t deadline;

    if (!out || !out_len || max_out == 0)
        return -1;

    *out = NULL;
    *out_len = 0;

    buf = malloc(max_out + 1);
    if (!buf)
        return -1;

    if (pipe(fds) < 0) {
        free(buf);
        return -1;
    }
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    fcntl(fds[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    if (!audio_debug_enabled())
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO,
                                         "/dev/null", O_WRONLY, 0);
    rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);

    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        free(buf);
        if (label)
            fprintf(stderr, "[Audio] %s konnte nicht gestartet werden: %s\n",
                    label, strerror(rc));
        return -1;
    }

    fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL, 0) | O_NONBLOCK);
    deadline = audio_now_ms() + timeout_ms;

    while (!(eof && child_done)) {
        for (;;) {
            char tmp[8192];
            ssize_t n = read(fds[0], tmp, sizeof(tmp));

            if (n > 0) {
                size_t copy = (size_t)n;

                if (copy > max_out - used) {
                    copy = max_out - used;
                    overflow = true;
                }
                if (copy > 0) {
                    memcpy(buf + used, tmp, copy);
                    used += copy;
                }
                continue;
            }
            if (n == 0) {
                eof = true;
                break;
            }
            if (errno == EINTR)
                continue;
            break; /* EAGAIN */
        }

        if (!child_done) {
            int status;
            pid_t r = waitpid(pid, &status, WNOHANG);

            if (r == pid) {
                child_done = true;
                child_status = status;
            } else if (r < 0 && errno != EINTR) {
                child_done = true;
            }
        }

        if (eof && child_done)
            break;
        if (audio_now_ms() >= deadline) {
            timed_out = true;
            break;
        }
        usleep(5000);
    }

    close(fds[0]);

    if (!child_done || timed_out) {
        bool killed_timeout = false;

        audio_reap(pid, 200, &child_status, &killed_timeout);
        timed_out = true;
    }

    buf[used] = '\0';

    if (timed_out) {
        if (label)
            fprintf(stderr, "[Audio] %s Zeitüberschreitung (%d ms)\n", label, timeout_ms);
        free(buf);
        return -2;
    }
    if (overflow) {
        if (label)
            fprintf(stderr, "[Audio] %s Ausgabe überschreitet %zu Bytes\n", label, max_out);
        free(buf);
        return -3;
    }
    if (!WIFEXITED(child_status)) {
        if (label)
            fprintf(stderr, "[Audio] %s wurde abnormal beendet\n", label);
        free(buf);
        return -1;
    }

    rc = WEXITSTATUS(child_status);
    if (rc != 0) {
        if (label)
            fprintf(stderr, "[Audio] %s fehlgeschlagen (Status %d)\n", label, rc);
        free(buf);
        return rc;
    }

    *out = buf;
    *out_len = used;
    return 0;
}

/* Runs argv, discards output and returns its exit code (-1 spawn error). */
static int audio_run(char *const argv[], int timeout_ms, const char *label)
{
    pid_t pid = 0;
    int status = 0;
    bool timed_out = false;
    int rc = audio_spawn(argv, -1, -1, true, false, &pid);

    if (rc != 0) {
        if (audio_debug_enabled())
            fprintf(stderr, "[Audio] %s konnte nicht gestartet werden: %s\n",
                    label, strerror(rc));
        return -1;
    }

    if (!audio_reap(pid, timeout_ms, &status, &timed_out))
        return -1;
    if (timed_out) {
        if (label && audio_debug_enabled())
            fprintf(stderr, "[Audio] %s Zeitüberschreitung (%d ms)\n", label, timeout_ms);
        return -2;
    }
    if (!WIFEXITED(status))
        return -1;

    return WEXITSTATUS(status);
}

static void stop_audio_pipeline(void)
{
    pid_t leader = g_audio_pid;
    pid_t second = g_audio_pid2;

    g_audio_pid = 0;
    g_audio_pid2 = 0;

    if (leader <= 0 && second <= 0)
        return;

    if (leader > 0) {
        kill(-leader, SIGTERM);
        kill(leader, SIGTERM);
    }
    if (second > 0)
        kill(second, SIGTERM);

    /* audio_reap() escalates to SIGKILL itself. */
    if (leader > 0)
        audio_reap(leader, 200, NULL, NULL);
    if (second > 0)
        audio_reap(second, 200, NULL, NULL);
}

/* Reads the PipeWire graph with `pw-dump Node` and parses it natively.
 * When a numeric --pw-node override is active, resolves it to a node.name. */
static bool audio_probe_fetch(struct audio_graph *graph,
                              char *override_name, size_t override_sz,
                              bool *have_override, bool quiet)
{
    char *json = NULL;
    size_t json_len = 0;
    char *argv[] = { (char *)"pw-dump", (char *)"Node", NULL };
    int rc;

    memset(graph, 0, sizeof(*graph));
    if (have_override)
        *have_override = false;

    rc = audio_capture(argv, &json, &json_len, AUDIO_PROBE_MAX_JSON,
                       AUDIO_PROBE_TIMEOUT_MS, quiet ? NULL : "pw-dump");
    if (rc != 0)
        return false;

    if (!audio_probe_graph(json, json_len, graph)) {
        free(json);
        return false;
    }

    if (g_pw_override_is_id && override_name && override_sz > 0) {
        if (audio_graph_find_name_by_id(json, json_len, g_pw_override_id,
                                        override_name, override_sz)) {
            if (have_override)
                *have_override = true;
        } else {
            fprintf(stderr, "[Audio] --pw-node ID %u nicht gefunden, nutze automatische Erkennung\n",
                    g_pw_override_id);
        }
    }

    free(json);
    return true;
}

static const char *audio_loop_bin(void)
{
    static char local_bin[600];
    char exe_path[512];
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path) - 1);

    if (access("/usr/local/bin/everlive-audio", X_OK) == 0)
        return "/usr/local/bin/everlive-audio";

    if (len > 0) {
        char *slash;

        exe_path[len] = '\0';
        slash = strrchr(exe_path, '/');
        if (slash)
            *slash = '\0';
        snprintf(local_bin, sizeof(local_bin), "%s/everlive-audio", exe_path);
        if (access(local_bin, X_OK) == 0)
            return local_bin;
    }

    return "pw-loopback";
}

static bool start_audio_pipeline(const char *adev, const char *target_node)
{
    if (g_audio_pid > 0 || g_audio_pid2 > 0)
        return true;

    if (target_node && target_node[0] != '\0') {
        const char *loop_bin = audio_loop_bin();
        char input_props[1024];
        char output_props[1200];
        char *argv[14];
        pid_t pid = 0;
        int rc;

        printf("[Audio] Starte PipeWire Audio-Pipeline...\n");
        printf("[Audio] Hinweis: Bitte Konsole auf 2-Kanal Linear PCM (Stereo) einstellen, um Aussetzer/Knacken zu vermeiden.\n");

        snprintf(input_props, sizeof(input_props),
                 "{ target.object = \"%s\" resample.disable = false application.name = \"EverLive\" application.icon-name = \"everlive\" application.id = \"everlive\" }",
                 target_node);
        snprintf(output_props, sizeof(output_props),
                 "{ node.virtual = false resample.disable = false resample.quality = 4 application.name = \"EverLive\" application.icon-name = \"everlive\" application.id = \"everlive\" application.process.id = %d application.process.binary = \"everlive_display\" node.name = \"EverLive\" node.description = \"EverLive Game Audio\" media.name = \"EverLive\" media.role = \"Game\" }",
                 (int)getpid());

        argv[0] = (char *)loop_bin;
        argv[1] = (char *)"-n";
        argv[2] = (char *)"EverLive";
        argv[3] = (char *)"-g";
        argv[4] = (char *)"EverLive";
        argv[5] = (char *)"-C";
        argv[6] = (char *)target_node;
        argv[7] = (char *)"-i";
        argv[8] = input_props;
        argv[9] = (char *)"-l";
        argv[10] = (char *)"60";
        argv[11] = (char *)"-o";
        argv[12] = output_props;
        argv[13] = NULL;

        rc = audio_spawn(argv, -1, -1, true, true, &pid);
        if (rc != 0) {
            fprintf(stderr, "[Audio] %s konnte nicht gestartet werden: %s\n",
                    loop_bin, strerror(rc));
            return false;
        }

        g_audio_pid = pid;
        return true;
    }

    /* ALSA fallback: arecord | pw-play, both explicit argv (no shell). */
    {
        char plug_dev[300];
        const char *dev = adev ? adev : "default";
        char play_props[1024];
        int fds[2];
        pid_t arec = 0;
        pid_t play = 0;
        int rc;

        if (strncmp(dev, "hw:", 3) == 0) {
            snprintf(plug_dev, sizeof(plug_dev), "plughw:%s", dev + 3);
            dev = plug_dev;
        }

        snprintf(play_props, sizeof(play_props),
                 "{ node.virtual = false application.name = \"EverLive\" application.icon-name = \"everlive\" application.id = \"everlive\" application.process.id = %d application.process.binary = \"everlive_display\" node.name = \"EverLive\" node.description = \"EverLive Game Audio\" media.name = \"EverLive\" media.role = \"Game\" }",
                 (int)getpid());

        {
            char *arecord_argv[] = {
                (char *)"arecord", (char *)"-D", (char *)dev,
                (char *)"-t", (char *)"wav", (char *)"-f", (char *)"S16_LE",
                (char *)"-c", (char *)"2", (char *)"-r", (char *)"48000",
                (char *)"--period-time=20000", (char *)"--buffer-time=250000",
                (char *)"-q", NULL
            };
            char *play_argv[] = {
                (char *)"pw-play", (char *)"--latency=60ms",
                (char *)"-P", play_props, (char *)"-", NULL
            };

            printf("[Audio] Starte ALSA-Fallback: arecord (%s) -> pw-play\n", dev);

            if (pipe(fds) < 0) {
                fprintf(stderr, "[Audio] pipe() fehlgeschlagen: %s\n", strerror(errno));
                return false;
            }
            fcntl(fds[0], F_SETFD, FD_CLOEXEC);
            fcntl(fds[1], F_SETFD, FD_CLOEXEC);

            rc = audio_spawn(arecord_argv, -1, fds[1], true, true, &arec);
            close(fds[1]);
            if (rc != 0) {
                close(fds[0]);
                fprintf(stderr, "[Audio] arecord konnte nicht gestartet werden: %s\n",
                        strerror(rc));
                return false;
            }

            rc = audio_spawn(play_argv, fds[0], -1, true, false, &play);
            close(fds[0]);
            if (rc != 0) {
                fprintf(stderr, "[Audio] pw-play konnte nicht gestartet werden: %s\n",
                        strerror(rc));
                kill(-arec, SIGTERM);
                kill(arec, SIGTERM);
                audio_reap(arec, 200, NULL, NULL);
                return false;
            }
        }

        g_audio_pid = arec;
        g_audio_pid2 = play;
        return true;
    }
}

static void audio_apply_volume(uint32_t source_id, uint32_t stream_id,
                               float volume, bool muted)
{
    char id[16];
    char vol[16];

    if (muted) {
        if (source_id > 0) {
            char *argv[] = { (char *)"wpctl", (char *)"set-mute", id, (char *)"1", NULL };

            snprintf(id, sizeof(id), "%u", source_id);
            (void)audio_run(argv, AUDIO_CMD_TIMEOUT_MS, "wpctl set-mute");
        }
        if (stream_id > 0) {
            char *argv[] = { (char *)"wpctl", (char *)"set-mute", id, (char *)"1", NULL };

            snprintf(id, sizeof(id), "%u", stream_id);
            (void)audio_run(argv, AUDIO_CMD_TIMEOUT_MS, "wpctl set-mute");
        }
        return;
    }

    if (audio_format_volume(volume, vol, sizeof(vol)) == 0)
        snprintf(vol, sizeof(vol), "1.00");

    if (source_id > 0) {
        char *unmute[] = { (char *)"wpctl", (char *)"set-mute", id, (char *)"0", NULL };
        char *setvol[] = { (char *)"wpctl", (char *)"set-volume", id, vol, NULL };

        snprintf(id, sizeof(id), "%u", source_id);
        if (audio_run(unmute, AUDIO_CMD_TIMEOUT_MS, "wpctl set-mute") == 0)
            (void)audio_run(setvol, AUDIO_CMD_TIMEOUT_MS, "wpctl set-volume");
    }

    if (stream_id > 0) {
        char *unmute[] = { (char *)"wpctl", (char *)"set-mute", id, (char *)"0", NULL };
        char *setvol[] = { (char *)"wpctl", (char *)"set-volume", id, vol, NULL };

        snprintf(id, sizeof(id), "%u", stream_id);
        if (audio_run(unmute, AUDIO_CMD_TIMEOUT_MS, "wpctl set-mute") == 0)
            (void)audio_run(setvol, AUDIO_CMD_TIMEOUT_MS, "wpctl set-volume");
    }
}

static bool audio_stream_alive(uint32_t id)
{
    char idbuf[16];
    char *argv[] = { (char *)"wpctl", (char *)"inspect", idbuf, NULL };
    int rc;

    snprintf(idbuf, sizeof(idbuf), "%u", id);
    rc = audio_run(argv, AUDIO_CMD_TIMEOUT_MS, "wpctl inspect");

    /* A missing/broken wpctl must not tear down a working stream. */
    if (rc < 0)
        return true;

    return rc == 0;
}

static void *audio_supervisor_thread(void *arg)
{
    const char *adev = (const char *)arg;
    struct audio_graph graph;
    char override_name[AUDIO_NODE_NAME_MAX] = "";
    bool have_override = false;
    const char *target_node = NULL;
    uint32_t stream_node_id = 0;
    uint32_t source_node_id = 0;
    float last_vol = -1.0f;
    bool last_muted = false;
    int fail_count = 0;
    time_t last_start_time = time(NULL);
    int64_t next_stream_probe;
    int stream_backoff = AUDIO_BACKOFF_MIN_MS;
    bool probed;

    /* Probe in the audio thread only: the video window must never wait for it. */
    probed = audio_probe_fetch(&graph, override_name, sizeof(override_name),
                               &have_override, false);

    if (!have_override && !g_pw_override_is_id && g_pw_override_name[0] != '\0') {
        snprintf(override_name, sizeof(override_name), "%s", g_pw_override_name);
        have_override = true;
    }

    if (!probed && !have_override)
        fprintf(stderr, "[Audio] PipeWire-Abfrage fehlgeschlagen, nutze ALSA-Fallback\n");

    if (have_override)
        target_node = override_name;
    else if (graph.have_source)
        target_node = graph.source.name;

    /* Keep the hardware source id even with an explicit --pw-node override so
     * volume/mute still reach the capture source. */
    if (graph.have_source)
        source_node_id = graph.source.id;

    if (target_node)
        printf("[Audio] Native PipeWire Loopback aktiv (Quelle: %s)\n", target_node);
    else
        printf("[Audio] Keine PipeWire-Quelle gefunden, ALSA-Fallback (%s)\n",
               adev ? adev : "default");

    if (graph.have_stream)
        stream_node_id = graph.stream.id;

    start_audio_pipeline(adev, target_node);
    g_audio_dirty = 1;
    next_stream_probe = audio_now_ms() + stream_backoff;

    while (g_running) {
        /* Explicit re-sync request (resolution change, stall recovery, hotkey) */
        if (g_audio_resync_needed) {
            g_audio_resync_needed = 0;
            printf("[Audio] Re-syncing audio stream...\n");
            stop_audio_pipeline();
            usleep(100000);
            stream_node_id = 0;
            next_stream_probe = 0;
            stream_backoff = AUDIO_BACKOFF_MIN_MS;
            if (g_running) {
                last_start_time = time(NULL);
                start_audio_pipeline(adev, target_node);
                g_audio_dirty = 1;
            }
        }

        /* Reap and restart a dead pipeline (either process of the pair). */
        {
            int status = 0;
            pid_t exited = 0;

            if (g_audio_pid > 0 && waitpid(g_audio_pid, &status, WNOHANG) == g_audio_pid) {
                exited = g_audio_pid;
                g_audio_pid = 0;
            } else if (g_audio_pid2 > 0 &&
                       waitpid(g_audio_pid2, &status, WNOHANG) == g_audio_pid2) {
                exited = g_audio_pid2;
                g_audio_pid2 = 0;
            }

            if (exited > 0) {
                printf("[Audio] Audio-Pipeline beendet (PID %d, Status %d), starte neu...\n",
                       (int)exited, status);
                /* The exited pid was already reaped; only stop the survivor. */
                stop_audio_pipeline();
                stream_node_id = 0;
                next_stream_probe = 0;
                stream_backoff = AUDIO_BACKOFF_MIN_MS;

                time_t now = time(NULL);
                if (now - last_start_time > 5)
                    fail_count = 0;
                fail_count++;

                /* Back off to prevent fork loops and speaker pops. */
                int backoff_sec = 1;
                if (fail_count > 5)
                    backoff_sec = 10;
                else if (fail_count > 2)
                    backoff_sec = 4;
                for (int s = 0; s < backoff_sec && g_running; s++)
                    sleep(1);

                if (g_running) {
                    last_start_time = time(NULL);
                    start_audio_pipeline(adev, target_node);
                    g_audio_dirty = 1;
                }
            }
        }

        /* Audio flow watchdog: is the stream node still reachable? */
        static int watchdog_counter = 0;
        if (stream_node_id > 0 && (++watchdog_counter % 80 == 0)) {
            if (!audio_stream_alive(stream_node_id)) {
                printf("[Audio] Stream-Node %u ungültig (Formatwechsel/Trennung), synchronisiere...\n",
                       stream_node_id);
                stream_node_id = 0;
                next_stream_probe = 0;
                g_audio_resync_needed = 1;
            }
        }

        /* Discover the EverLive playback stream; back off on repeated misses. */
        if (stream_node_id == 0 && g_audio_pid > 0 &&
            audio_now_ms() >= next_stream_probe) {
            struct audio_graph refresh;

            if (audio_probe_fetch(&refresh, NULL, 0, NULL, true)) {
                if (source_node_id == 0 && refresh.have_source)
                    source_node_id = refresh.source.id;
                if (refresh.have_stream) {
                    stream_node_id = refresh.stream.id;
                    printf("[Audio] PipeWire Audio-Stream aktiv (Node ID: %u)\n",
                           stream_node_id);
                    g_audio_dirty = 1;
                    stream_backoff = AUDIO_BACKOFF_MIN_MS;
                }
            }

            if (stream_node_id == 0) {
                next_stream_probe = audio_now_ms() + stream_backoff;
                stream_backoff *= 2;
                if (stream_backoff > AUDIO_BACKOFF_MAX_MS)
                    stream_backoff = AUDIO_BACKOFF_MAX_MS;
            }
        }

        /* Apply volume or mute updates. */
        if ((stream_node_id > 0 || source_node_id > 0) &&
            (g_audio_dirty || g_volume != last_vol || g_muted != last_muted)) {
            g_audio_dirty = 0;
            last_vol = g_volume;
            last_muted = g_muted;
            audio_apply_volume(source_node_id, stream_node_id, g_volume, g_muted);
        }

        usleep(25000);
    }

    stop_audio_pipeline();
    return NULL;
}

/* 8x8 Bitmap Text Rendering (horizontal run-length batched for high performance) */
static void draw_char(SDL_Renderer *r, char c, int x, int y, int scale, SDL_Color color)
{
    uint8_t uc = (uint8_t)c;
    if (uc > 127) uc = '?';
    const uint8_t *bitmap = font8x8_basic[uc];
    SDL_SetRenderDrawColor(r, color.r, color.g, color.b, color.a);
    for (int row = 0; row < 8; row++) {
        uint8_t b = bitmap[row];
        int col = 0;
        while (col < 8) {
            if ((b >> col) & 1) {
                int start_col = col;
                while (col < 8 && ((b >> col) & 1))
                    col++;
                SDL_Rect rc = { x + start_col * scale, y + row * scale, (col - start_col) * scale, scale };
                SDL_RenderFillRect(r, &rc);
            } else {
                col++;
            }
        }
    }
}

static void draw_string(SDL_Renderer *r, const char *str, int x, int y, int scale, SDL_Color color)
{
    while (*str) {
        draw_char(r, *str, x, y, scale, color);
        x += 8 * scale;
        str++;
    }
}

static void draw_string_centered(SDL_Renderer *r, const char *str, int center_x, int y, int scale, SDL_Color color)
{
    int len = 0;
    const char *p = str;
    while (*p++) len++;
    int total_w = len * 8 * scale;
    draw_string(r, str, center_x - total_w / 2, y, scale, color);
}

static void update_fullscreen(SDL_Window *window, bool enable)
{
    g_fullscreen = enable;
    if (enable) {
        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "1");
        SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);
    } else {
        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
        SDL_SetWindowFullscreen(window, 0);
    }
}

struct stream_ctx {
    int fd;
    bool active;
    uint32_t width;
    uint32_t height;
    uint32_t preferred_pixelformat;
    uint32_t pixelformat;
    uint32_t bytesperline;
    uint32_t sdl_format;
    bool interlaced;
    bool is_hdr;
    bool is_hlg;
    bool is_full_range;
    struct v4l2_mmap_buffer buffers[BUFFER_COUNT];
    unsigned int buffer_count;
    SDL_Texture *texture;
    GLuint gl_prog;
    GLuint gl_tex_y;
    GLuint gl_tex_uv;
    GLint u_loc_tex_y;
    GLint u_loc_tex_uv;
    GLint u_loc_resolution;
    GLint u_loc_fmt_type;
    GLint u_loc_is_hdr;
    GLint u_loc_is_hlg;
    GLint u_loc_is_full_range;
    GLint u_loc_exposure;
    GLint u_loc_tonemap_mode;
    bool gl_ready;
};

static const char *g_hdr_vs_src =
    "#version 130\n"
    "in vec2 in_pos;\n"
    "in vec2 in_uv;\n"
    "out vec2 v_uv;\n"
    "void main() {\n"
    "    gl_Position = vec4(in_pos, 0.0, 1.0);\n"
    "    v_uv = in_uv;\n"
    "}\n";

static const char *g_hdr_fs_src =
    "#version 130\n"
    "uniform sampler2D u_tex_y;\n"
    "uniform sampler2D u_tex_uv;\n"
    "uniform vec2 u_resolution;\n"
    "uniform int u_fmt_type;\n"       // 0 = YUYV, 1 = NV12, 2 = RGB
    "uniform int u_is_hdr;\n"         // 1 = HDR active
    "uniform int u_is_hlg;\n"         // 1 = HLG, 0 = PQ (HDR10)
    "uniform int u_is_full_range;\n"  // 1 = Full Range [0..255], 0 = Limited [16..235]
    "uniform float u_exposure;\n"     // Tone mapping scale
    "uniform int u_tonemap_mode;\n"   // 0 = SDR Tonemap (ACES), 1 = HDR Monitor Passthrough (Native)
    "in vec2 v_uv;\n"
    "out vec4 frag_color;\n"
    "\n"
    "vec3 pq_to_linear(vec3 N) {\n"
    "    const float m1 = 0.1593017578125;\n"
    "    const float m2 = 78.84375;\n"
    "    const float c1 = 0.8359375;\n"
    "    const float c2 = 18.8515625;\n"
    "    const float c3 = 18.6875;\n"
    "    vec3 N_inv_m2 = pow(clamp(N, 0.0, 1.0), vec3(1.0 / m2));\n"
    "    vec3 num = max(N_inv_m2 - c1, 0.0);\n"
    "    vec3 den = max(c2 - c3 * N_inv_m2, 0.00001);\n"
    "    return pow(num / den, vec3(1.0 / m1));\n"
    "}\n"
    "\n"
    "vec3 hlg_to_linear(vec3 E) {\n"
    "    const float a = 0.17883277;\n"
    "    const float b = 0.28466892;\n"
    "    const float c = 0.55991073;\n"
    "    vec3 lin;\n"
    "    for (int i = 0; i < 3; i++) {\n"
    "        float v = clamp(E[i], 0.0, 1.0);\n"
    "        if (v <= 0.5) lin[i] = (v * v) / 3.0;\n"
    "        else lin[i] = (exp((v - c) / a) + b) / 12.0;\n"
    "    }\n"
    "    return lin;\n"
    "}\n"
    "\n"
    "vec3 tonemap_aces(vec3 x) {\n"
    "    const float a = 2.51;\n"
    "    const float b = 0.03;\n"
    "    const float c = 2.43;\n"
    "    const float d = 0.59;\n"
    "    const float e = 0.14;\n"
    "    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);\n"
    "}\n"
    "\n"
    "void main() {\n"
    "    if (u_fmt_type == 2) {\n"
    "        vec3 col = texture(u_tex_y, v_uv).rgb;\n"
    "        frag_color = vec4(col, 1.0);\n"
    "        return;\n"
    "    }\n"
    "\n"
    "    float y = 0.0, u = 0.5, v = 0.5;\n"
    "    if (u_fmt_type == 0) {\n"
    "        int col_idx = int(v_uv.x * u_resolution.x);\n"
    "        int row_idx = int(v_uv.y * u_resolution.y);\n"
    "        vec4 texel = texelFetch(u_tex_y, ivec2(col_idx >> 1, row_idx), 0);\n"
    "        y = ((col_idx & 1) == 0) ? texel.r : texel.b;\n"
    "        u = texel.g;\n"
    "        v = texel.a;\n"
    "    } else if (u_fmt_type == 1) {\n"
    "        y = texture(u_tex_y, v_uv).r;\n"
    "        vec2 uv = texture(u_tex_uv, v_uv).rg;\n"
    "        u = uv.r;\n"
    "        v = uv.g;\n"
    "    }\n"
    "\n"
    "    float y_norm, u_norm, v_norm;\n"
    "    if (u_is_full_range != 0) {\n"
    "        y_norm = clamp(y, 0.0, 1.0);\n"
    "        u_norm = clamp(u - 0.5, -0.5, 0.5);\n"
    "        v_norm = clamp(v - 0.5, -0.5, 0.5);\n"
    "    } else {\n"
    "        y_norm = clamp((y - (16.0 / 255.0)) / (219.0 / 255.0), 0.0, 1.0);\n"
    "        u_norm = clamp((u - (128.0 / 255.0)) / (224.0 / 255.0), -0.5, 0.5);\n"
    "        v_norm = clamp((v - (128.0 / 255.0)) / (224.0 / 255.0), -0.5, 0.5);\n"
    "    }\n"
    "\n"
    "    if (u_is_hdr == 0) {\n"
    "        vec3 rgb;\n"
    "        rgb.r = y_norm + 1.57480 * v_norm;\n"
    "        rgb.g = y_norm - 0.18732 * u_norm - 0.46812 * v_norm;\n"
    "        rgb.b = y_norm + 1.85560 * u_norm;\n"
    "        frag_color = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
    "        return;\n"
    "    }\n"
    "\n"
    "    vec3 rgb_2020;\n"
    "    rgb_2020.r = y_norm + 1.47460 * v_norm;\n"
    "    rgb_2020.g = y_norm - 0.16455 * u_norm - 0.57135 * v_norm;\n"
    "    rgb_2020.b = y_norm + 1.88140 * u_norm;\n"
    "    rgb_2020 = clamp(rgb_2020, 0.0, 1.0);\n"
    "\n"
    "    if (u_tonemap_mode == 1) {\n"
    "        frag_color = vec4(rgb_2020, 1.0);\n"
    "        return;\n"
    "    }\n"
    "\n"
    "    vec3 lin_2020;\n"
    "    if (u_is_hlg != 0) {\n"
    "        lin_2020 = hlg_to_linear(rgb_2020);\n"
    "    } else {\n"
    "        lin_2020 = pq_to_linear(rgb_2020);\n"
    "    }\n"
    "\n"
    "    const mat3 mat_bt2020_to_bt709 = mat3(\n"
    "         1.6605, -0.1246, -0.0182,\n"
    "        -0.5876,  1.1329, -0.1006,\n"
    "        -0.0728, -0.0083,  1.1187\n"
    "    );\n"
    "    vec3 lin_709 = max(mat_bt2020_to_bt709 * lin_2020, vec3(0.0));\n"
    "    vec3 mapped = tonemap_aces(lin_709 * u_exposure);\n"
    "    vec3 sdr = pow(mapped, vec3(1.0 / 2.2));\n"
    "    frag_color = vec4(clamp(sdr, 0.0, 1.0), 1.0);\n"
    "}\n";

static void cleanup_gl_pipeline(struct stream_ctx *ctx)
{
    if (ctx->gl_tex_y) {
        glDeleteTextures(1, &ctx->gl_tex_y);
        ctx->gl_tex_y = 0;
    }
    if (ctx->gl_tex_uv) {
        glDeleteTextures(1, &ctx->gl_tex_uv);
        ctx->gl_tex_uv = 0;
    }
    if (ctx->gl_prog) {
        glDeleteProgram(ctx->gl_prog);
        ctx->gl_prog = 0;
    }
    ctx->gl_ready = false;
}

static bool init_gl_pipeline(struct stream_ctx *ctx)
{
    if (ctx->gl_ready) return true;

    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &g_hdr_vs_src, NULL);
    glCompileShader(vs);
    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(vs);
        return false;
    }

    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &g_hdr_fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return false;
    }

    ctx->gl_prog = glCreateProgram();
    glAttachShader(ctx->gl_prog, vs);
    glAttachShader(ctx->gl_prog, fs);
    glBindAttribLocation(ctx->gl_prog, 0, "in_pos");
    glBindAttribLocation(ctx->gl_prog, 1, "in_uv");
    glLinkProgram(ctx->gl_prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    glGetProgramiv(ctx->gl_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        glDeleteProgram(ctx->gl_prog);
        ctx->gl_prog = 0;
        return false;
    }

    if (ctx->pixelformat == V4L2_PIX_FMT_YUYV) {
        glGenTextures(1, &ctx->gl_tex_y);
        glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_y);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, (GLsizei)(ctx->width / 2), (GLsizei)ctx->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    } else if (ctx->pixelformat == V4L2_PIX_FMT_NV12) {
        glGenTextures(1, &ctx->gl_tex_y);
        glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_y);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)ctx->width, (GLsizei)ctx->height, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);

        glGenTextures(1, &ctx->gl_tex_uv);
        glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_uv);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, (GLsizei)(ctx->width / 2), (GLsizei)(ctx->height / 2), 0, GL_RG, GL_UNSIGNED_BYTE, NULL);
    } else {
        glGenTextures(1, &ctx->gl_tex_y);
        glBindTexture(GL_TEXTURE_2D, ctx->gl_tex_y);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        GLenum internal = (ctx->pixelformat == V4L2_PIX_FMT_RGB32) ? GL_RGBA8 : GL_RGB8;
        GLenum fmt = (ctx->pixelformat == V4L2_PIX_FMT_RGB32) ? GL_BGRA : GL_BGR;
        glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, (GLsizei)ctx->width, (GLsizei)ctx->height, 0, fmt, GL_UNSIGNED_BYTE, NULL);
    }

    ctx->u_loc_tex_y = glGetUniformLocation(ctx->gl_prog, "u_tex_y");
    ctx->u_loc_tex_uv = glGetUniformLocation(ctx->gl_prog, "u_tex_uv");
    ctx->u_loc_resolution = glGetUniformLocation(ctx->gl_prog, "u_resolution");
    ctx->u_loc_fmt_type = glGetUniformLocation(ctx->gl_prog, "u_fmt_type");
    ctx->u_loc_is_hdr = glGetUniformLocation(ctx->gl_prog, "u_is_hdr");
    ctx->u_loc_is_hlg = glGetUniformLocation(ctx->gl_prog, "u_is_hlg");
    ctx->u_loc_is_full_range = glGetUniformLocation(ctx->gl_prog, "u_is_full_range");
    ctx->u_loc_exposure = glGetUniformLocation(ctx->gl_prog, "u_exposure");
    ctx->u_loc_tonemap_mode = glGetUniformLocation(ctx->gl_prog, "u_tonemap_mode");

    ctx->gl_ready = true;
    printf("[*] Zero-Latency GPU HDR Pipeline active (HDR Passthrough & ACES Tone-Mapping ready)\n");
    return true;
}

static void stream_stop(struct stream_ctx *ctx)
{
    if (!ctx) return;
    cleanup_gl_pipeline(ctx);
    if (ctx->active) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
        ctx->active = false;
    }
    for (unsigned int i = 0; i < ctx->buffer_count; i++) {
        if (ctx->buffers[i].start && ctx->buffers[i].start != MAP_FAILED) {
            munmap(ctx->buffers[i].start, ctx->buffers[i].length);
            ctx->buffers[i].start = NULL;
        }
    }
    if (ctx->buffer_count > 0) {
        struct v4l2_requestbuffers req = {};
        req.count = 0;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        ioctl(ctx->fd, VIDIOC_REQBUFS, &req);
        ctx->buffer_count = 0;
    }
    if (ctx->texture) {
        SDL_DestroyTexture(ctx->texture);
        ctx->texture = NULL;
    }
}

static const char *get_chroma_desc(uint32_t fourcc)
{
    switch (fourcc) {
    case V4L2_PIX_FMT_BGR24:
        return "RGB 4:4:4";
    case V4L2_PIX_FMT_RGB32:
        return "RGB 4:4:4";
    case V4L2_PIX_FMT_YUYV:
        return "YUV 4:2:2";
    case V4L2_PIX_FMT_NV12:
        return "YUV 4:2:0";
    case V4L2_PIX_FMT_P010:
        return "P010 4:2:0";
    default:
        return "YUV 4:2:2";
    }
}

static bool stream_start(struct stream_ctx *ctx, SDL_Renderer *renderer)
{
    stream_stop(ctx);

    /* Hardware DV-Timings query and synchronization */
    struct v4l2_dv_timings dv = {};
    bool dv_interlaced = false;
    if (ioctl(ctx->fd, VIDIOC_QUERY_DV_TIMINGS, &dv) == 0 && dv.bt.width > 0 && dv.bt.height > 0) {
        dv_interlaced = (dv.bt.interlaced == V4L2_DV_INTERLACED);
        ioctl(ctx->fd, VIDIOC_S_DV_TIMINGS, &dv);
        struct v4l2_format sfmt = {};
        sfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        sfmt.fmt.pix.width = dv.bt.width;
        sfmt.fmt.pix.height = dv.bt.height;
        sfmt.fmt.pix.pixelformat = ctx->preferred_pixelformat ? ctx->preferred_pixelformat : V4L2_PIX_FMT_YUYV;
        sfmt.fmt.pix.field = dv_interlaced ? V4L2_FIELD_INTERLACED : V4L2_FIELD_NONE;
        ioctl(ctx->fd, VIDIOC_S_FMT, &sfmt);
    } else {
        /* Timings not locked yet - stay in standby, do not attempt streaming with stale format */
        return false;
    }

    struct v4l2_format fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(ctx->fd, VIDIOC_G_FMT, &fmt) < 0) {
        return false;
    }

    ctx->width = fmt.fmt.pix.width;
    ctx->height = fmt.fmt.pix.height;
    ctx->pixelformat = fmt.fmt.pix.pixelformat;
    ctx->bytesperline = fmt.fmt.pix.bytesperline;
    ctx->interlaced = dv_interlaced ||
                      fmt.fmt.pix.field == V4L2_FIELD_INTERLACED ||
                      fmt.fmt.pix.field == V4L2_FIELD_INTERLACED_TB ||
                      fmt.fmt.pix.field == V4L2_FIELD_INTERLACED_BT;
    ctx->is_hdr = (fmt.fmt.pix.colorspace == V4L2_COLORSPACE_BT2020 ||
                   fmt.fmt.pix.xfer_func == V4L2_XFER_FUNC_SMPTE2084);
    ctx->is_hlg = (fmt.fmt.pix.colorspace == V4L2_COLORSPACE_BT2020 &&
                   fmt.fmt.pix.xfer_func != V4L2_XFER_FUNC_SMPTE2084);
    ctx->is_full_range = (fmt.fmt.pix.quantization == V4L2_QUANTIZATION_FULL_RANGE);

    if (ctx->width == 0 || ctx->height == 0) {
        return false;
    }

    if (ctx->pixelformat == V4L2_PIX_FMT_YUYV)
        ctx->sdl_format = SDL_PIXELFORMAT_YUY2;
    else if (ctx->pixelformat == V4L2_PIX_FMT_NV12)
        ctx->sdl_format = SDL_PIXELFORMAT_NV12;
    else if (ctx->pixelformat == V4L2_PIX_FMT_BGR24)
        ctx->sdl_format = SDL_PIXELFORMAT_BGR24;
    else if (ctx->pixelformat == V4L2_PIX_FMT_RGB32)
        ctx->sdl_format = SDL_PIXELFORMAT_ARGB8888;
    else
        ctx->sdl_format = SDL_PIXELFORMAT_YUY2;

    struct v4l2_requestbuffers req = {};
    req.count = BUFFER_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) {
        return false;
    }
    ctx->buffer_count = req.count;

    for (unsigned int i = 0; i < req.count; i++) {
        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            stream_stop(ctx);
            return false;
        }
        ctx->buffers[i].length = buf.length;
        ctx->buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, ctx->fd, buf.m.offset);
        if (ctx->buffers[i].start == MAP_FAILED) {
            stream_stop(ctx);
            return false;
        }
        if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            stream_stop(ctx);
            return false;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(ctx->fd, VIDIOC_STREAMON, &type) < 0) {
        stream_stop(ctx);
        return false;
    }

    ctx->texture = SDL_CreateTexture(
        renderer,
        ctx->sdl_format,
        SDL_TEXTUREACCESS_STREAMING,
        (int)ctx->width, (int)ctx->height
    );
    if (!ctx->texture) {
        stream_stop(ctx);
        return false;
    }

    init_gl_pipeline(ctx);

    /* Drain any stale buffers from V4L2 queue for minimal latency */
    struct v4l2_buffer drain_buf = {};
    drain_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    drain_buf.memory = V4L2_MEMORY_MMAP;
    while (ioctl(ctx->fd, VIDIOC_DQBUF, &drain_buf) == 0) {
        ioctl(ctx->fd, VIDIOC_QBUF, &drain_buf);
        memset(&drain_buf, 0, sizeof(drain_buf));
        drain_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        drain_buf.memory = V4L2_MEMORY_MMAP;
    }

    ctx->active = true;
    printf("[+] HDMI Stream active: %ux%u%c [%s%s%s] (%u bytes/line)\n",
           ctx->width, ctx->height, ctx->interlaced ? 'i' : 'p',
           get_chroma_desc(ctx->pixelformat),
           ctx->is_hdr ? (ctx->is_hlg ? " HLG" : " HDR10") : "",
           ctx->is_full_range ? " FullRange" : " LimitedRange",
           ctx->bytesperline);
    return true;
}

int main(int argc, char *argv[])
{
    const char *vdev = NULL;
    const char *adev = NULL;
    bool start_fullscreen = false;
    bool enable_vsync = true;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") || !strcmp(argv[i], "--fullscreen"))
            start_fullscreen = true;
        else if (!strcmp(argv[i], "--vsync"))
            enable_vsync = true;
        else if (!strcmp(argv[i], "--no-vsync"))
            enable_vsync = false;
        else if (!strcmp(argv[i], "-s") || !strcmp(argv[i], "--streamer-mode") || !strcmp(argv[i], "--clean"))
            g_streamer_mode = true;
        else if (!strcmp(argv[i], "-d") && i + 1 < argc)
            vdev = argv[++i];
        else if (!strcmp(argv[i], "-a") && i + 1 < argc)
            adev = argv[++i];
        else if ((!strcmp(argv[i], "-w") || !strcmp(argv[i], "--wid")) && i + 1 < argc)
            g_embed_wid = (uintptr_t)strtoull(argv[++i], NULL, 0);
        else if ((!strcmp(argv[i], "-v") || !strcmp(argv[i], "--volume")) && i + 1 < argc) {
            float v = strtof(argv[++i], NULL);
            if (v >= 0.0f && v <= 1.5f) {
                g_volume = v;
            }
        }
        else if (!strcmp(argv[i], "--pw-node") && i + 1 < argc) {
            const char *node = argv[++i];
            uint32_t node_id = 0;

            if (audio_parse_node_id(node, &node_id)) {
                g_pw_override_id = node_id;
                g_pw_override_is_id = true;
            } else if (audio_validate_node_name(node)) {
                snprintf(g_pw_override_name, sizeof(g_pw_override_name), "%s", node);
            } else {
                fprintf(stderr, "[Audio] Ungültiger --pw-node Wert '%s' ignoriert "
                                "(erlaubt: [A-Za-z0-9._:+-] oder numerische ID)\n", node);
            }
        }
        else if (!strcmp(argv[i], "--no-audio") || !strcmp(argv[i], "-n"))
            adev = "none";
        else if ((!strcmp(argv[i], "-F") || !strcmp(argv[i], "--format")) && i + 1 < argc) {
            const char *f = argv[++i];
            if (!strcasecmp(f, "yuyv") || !strcasecmp(f, "yuy2"))
                g_preferred_fmt = V4L2_PIX_FMT_YUYV;
            else if (!strcasecmp(f, "nv12"))
                g_preferred_fmt = V4L2_PIX_FMT_NV12;
            else if (!strcasecmp(f, "bgr24") || !strcasecmp(f, "bgr3") || !strcasecmp(f, "rgb"))
                g_preferred_fmt = V4L2_PIX_FMT_BGR24;
            else if (!strcasecmp(f, "rgb32") || !strcasecmp(f, "rgb4") || !strcasecmp(f, "bgrx") || !strcasecmp(f, "rgba"))
                g_preferred_fmt = V4L2_PIX_FMT_RGB32;
        }
        else if (!strcmp(argv[i], "--hdr") && i + 1 < argc) {
            const char *h = argv[++i];
            if (!strcasecmp(h, "off") || !strcasecmp(h, "sdr") || !strcasecmp(h, "0"))
                g_hdr_mode = 2;
            else if (!strcasecmp(h, "on") || !strcasecmp(h, "1"))
                g_hdr_mode = 1;
            else
                g_hdr_mode = 0;
        }
        else if (!strcmp(argv[i], "--exposure") && i + 1 < argc) {
            float exp_val = strtof(argv[++i], NULL);
            if (exp_val >= 1.0f && exp_val <= 40.0f) {
                g_hdr_exposure = exp_val;
            }
        }
        else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            printf("Usage: %s [-f|--fullscreen] [--vsync] [-s|--streamer-mode] [--hdr auto|on|off] [--exposure 1.0-40.0] [-F yuyv|nv12|bgr24|rgb32] [-d /dev/videoX] [-a hw:CARD,DEV|none] [--pw-node name|id] [-w wid] [--no-audio]\n", argv[0]);
            return 0;
        }
    }

    /* Mesa / AMD Radeon GPU optimization */
    if (enable_vsync) {
        setenv("vblank_mode", "1", 1);
    } else {
        setenv("vblank_mode", "0", 1);
    }
    setenv("mesa_glthread", "true", 1);

    if (g_embed_wid > 0) {
        setenv("SDL_VIDEODRIVER", "x11", 1);
        g_hud_visible = false;
        g_show_stats = true; /* Keep stats HUD visible so user sees real-time FPS/Lag/Jitter */
    }

    if (!vdev)
        vdev = detect_vdev();
    if (!vdev) {
        fprintf(stderr, "[Error] Kein GC573 V4L2-Gerät gefunden. Bitte prüfen, ob der Treiber geladen ist ('lsmod | grep gc573').\n");
        return 1;
    }
    if (!adev)
        adev = detect_adev();

    /* Audio routing is probed in the audio thread so the window never waits
     * for pw-dump.  Only the validated override is reported here. */
    if (adev && strcmp(adev, "none") != 0) {
        if (g_pw_override_is_id)
            printf("[Audio] Node-ID override: %u\n", g_pw_override_id);
        else if (g_pw_override_name[0] != '\0')
            printf("[Audio] Node override: %s\n", g_pw_override_name);
    }

    /* Applied to every PipeWire client we spawn (pw-loopback / pw-play). */
    setenv("PIPEWIRE_PROPS",
           "{ application.name = \"EverLive\" application.icon-name = \"everlive\" application.id = \"everlive\" }",
           1);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGUSR1, sigusr1_handler);
    signal(SIGWINCH, sigwinch_handler);

    int fd = open(vdev, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "[Error] Konnte V4L2 Gerät '%s' nicht öffnen: %s\n", vdev, strerror(errno));
        return 1;
    }

    /* Versuche ein initiales Signal abzufragen fuer optimale Standard-Fenstergroesse */
    uint32_t init_w = 1920, init_h = 1080;
    struct v4l2_dv_timings init_dv = {};
    if (ioctl(fd, VIDIOC_QUERY_DV_TIMINGS, &init_dv) == 0 && init_dv.bt.width > 0 && init_dv.bt.height > 0) {
        init_w = init_dv.bt.width;
        init_h = init_dv.bt.height;
        ioctl(fd, VIDIOC_S_DV_TIMINGS, &init_dv);
    } else {
        struct v4l2_format init_fmt = {};
        init_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (ioctl(fd, VIDIOC_G_FMT, &init_fmt) == 0 && init_fmt.fmt.pix.width > 0 && init_fmt.fmt.pix.height > 0) {
            init_w = init_fmt.fmt.pix.width;
            init_h = init_fmt.fmt.pix.height;
        }
    }

    /* Initialize SDL2 */
    SDL_SetHint(SDL_HINT_APP_NAME, "EverLive");
    if (g_embed_wid > 0) {
        setenv("SDL_VIDEODRIVER", "x11", 1);
    }
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "[Error] SDL_Init fehlgeschlagen: %s\n", SDL_GetError());
        close(fd);
        return 1;
    }

    /* Hints for lowest latency on Wayland & X11 */
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, enable_vsync ? "1" : "0");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
    SDL_SetHint(SDL_HINT_RENDER_BATCHING, "1");
    SDL_SetHint("SDL_VIDEO_WAYLAND_TEARING", enable_vsync ? "0" : "1");
    SDL_SetHint("SDL_VIDEO_WAYLAND_ALLOW_DATACONTROL", "1");
    if (start_fullscreen) {
        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "1");
    } else {
        SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    }
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    SDL_SetHint("SDL_X11_WM_CLASS", "EverLive");
    SDL_SetHint("SDL_VIDEO_WAYLAND_WM_CLASS", "EverLive");

    SDL_Window *window = NULL;
    if (g_embed_wid > 0) {
        window = SDL_CreateWindowFrom((void *)g_embed_wid);
    } else {
        int win_w = (int)init_w;
        int win_h = (init_w == 3840 && init_h == 1080) ? 2160 : (int)init_h;
        if (init_w == 1440 && (init_h == 480 || init_h == 576)) {
            win_w = 720;
        } else if (init_w == 2880 && init_h == 240) {
            win_w = 640;
            win_h = 480;
        } else if (init_w == 1440 && init_h == 240) {
            win_w = 640;
            win_h = 480;
        } else if (init_w == 1920 && init_h == 240) {
            win_w = 640;
            win_h = 480;
        }
        Uint32 win_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_OPENGL;
        if (start_fullscreen) {
            win_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
            g_fullscreen = true;
        }

        /* In multi-monitor setups, select matching display */
        int target_display = 0;
        int num_displays = SDL_GetNumVideoDisplays();
        for (int d = 0; d < num_displays; d++) {
            SDL_DisplayMode dm;
            if (SDL_GetCurrentDisplayMode(d, &dm) == 0) {
                if ((uint32_t)dm.w == init_w && (uint32_t)dm.h == init_h) {
                    target_display = d;
                    break;
                }
            }
        }

        /* In windowed mode, ensure window fits inside display without overflowing */
        if (!start_fullscreen) {
            SDL_Rect bounds;
            if (SDL_GetDisplayBounds(target_display, &bounds) == 0) {
                int max_w = bounds.w * 85 / 100;
                int max_h = bounds.h * 85 / 100;
                if (win_w > max_w || win_h > max_h) {
                    float aspect = (float)win_w / (float)win_h;
                    if ((float)max_w / aspect <= (float)max_h) {
                        win_w = max_w;
                        win_h = (int)((float)max_w / aspect);
                    } else {
                        win_h = max_h;
                        win_w = (int)((float)max_h * aspect);
                    }
                }
            }
        }

        window = SDL_CreateWindow(
            "EverLive - Console Gaming Screen [INTERN TEST - EXPERIMENTAL]",
            SDL_WINDOWPOS_CENTERED_DISPLAY(target_display), SDL_WINDOWPOS_CENTERED_DISPLAY(target_display),
            win_w, win_h,
            win_flags
        );
        if (window) {
            char exe_dir[512] = "";
            ssize_t len = readlink("/proc/self/exe", exe_dir, sizeof(exe_dir) - 1);
            if (len > 0) {
                exe_dir[len] = '\0';
                char *slash = strrchr(exe_dir, '/');
                if (slash) *slash = '\0';
            }
            char icon_path[600];
            snprintf(icon_path, sizeof(icon_path), "%s/everlive.bmp", exe_dir);
            SDL_Surface *icon = SDL_LoadBMP(icon_path);
            if (!icon) {
                icon = SDL_LoadBMP("tools/everlive/everlive.bmp");
            }
            if (icon) {
                SDL_SetWindowIcon(window, icon);
                SDL_FreeSurface(icon);
            }

            /* Explicitly set X11 WM_CLASS to 'EverLive' to match everlive.desktop in Dock and App Switcher */
            SDL_SysWMinfo wmInfo;
            SDL_VERSION(&wmInfo.version);
            if (SDL_GetWindowWMInfo(window, &wmInfo) && wmInfo.subsystem == SDL_SYSWM_X11) {
                XClassHint hint;
                hint.res_name = (char *)"everlive";
                hint.res_class = (char *)"EverLive";
                XSetClassHint(wmInfo.info.x11.display, wmInfo.info.x11.window, &hint);
            }
        }
        if (start_fullscreen && window) {
            update_fullscreen(window, true);
        }
    }
    if (!window) {
        fprintf(stderr, "[Error] Fenstererstellung fehlgeschlagen: %s\n", SDL_GetError());
        SDL_Quit();
        close(fd);
        return 1;
    }

    Uint32 rflags = SDL_RENDERER_ACCELERATED;
    if (enable_vsync) {
        rflags |= SDL_RENDERER_PRESENTVSYNC;
    }
    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1, rflags);
    if (!renderer) {
        fprintf(stderr, "[Error] SDL_CreateRenderer fehlgeschlagen: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        close(fd);
        return 1;
    }

    /* Set swap interval: 1 if VSync is enabled, or 0 (unthrottled) for lowest input lag */
    SDL_GL_SetSwapInterval(enable_vsync ? 1 : 0);

    SDL_SetRenderDrawColor(renderer, 13, 17, 23, 255);
    SDL_RenderClear(renderer);
    SDL_RenderPresent(renderer);
    if (g_embed_wid == 0) {
        SDL_RaiseWindow(window);
    }

    struct stream_ctx ctx = { .fd = fd, .preferred_pixelformat = g_preferred_fmt };
    if (!stream_start(&ctx, renderer)) {
        printf("[*] No HDMI signal at launch - standby mode active (waiting for signal)...\n");
    }

    /* Setup Audio Pipeline (PipeWire 10ms with native surround mixing) */
    bool enable_audio = adev && strcmp(adev, "none") != 0;
    pthread_t audio_tid;

    if (enable_audio) {
        pthread_create(&audio_tid, NULL, audio_supervisor_thread, (void *)adev);
    }

    printf("[+] Direct Display active! Hotkeys: [F11/f] Fullscreen, [m] Mute, [Scroll] Volume, [Tab] Stats, [h/r] Re-Sync, [Esc/q] Exit\n");

    /* Subscribe to driver resolution change events for seamless auto-switching */
    struct v4l2_event_subscription sub = {};
    sub.type = V4L2_EVENT_SOURCE_CHANGE;
    ioctl(fd, VIDIOC_SUBSCRIBE_EVENT, &sub);

    struct pollfd pfd = { .fd = fd, .events = POLLIN | POLLPRI };
    uint64_t frame_count = 0;
    uint32_t last_title_time = SDL_GetTicks();
    uint32_t last_retry_time = SDL_GetTicks();
    uint32_t last_frame_ticks = SDL_GetTicks();
    g_last_mouse_time = SDL_GetTicks();

    double lat_ms = 0.0;
    double live_fps = 0.0;
    uint32_t live_jitter = 0;
    uint32_t jitter_count = 0;

    uint64_t sum_poll_us = 0;
    uint64_t sum_dqbuf_us = 0;
    uint64_t sum_upload_us = 0;
    uint64_t sum_draw_us = 0;
    uint64_t sum_present_us = 0;
    uint32_t perf_samples = 0;

    bool dragging_volume = false;
    int cur_w = 0, cur_h = 0;

    while (g_running) {
        uint32_t now_ticks = SDL_GetTicks();
        if (g_need_geom_check > 0) {
            g_need_geom_check--;
            if (g_embed_wid > 0) {
                Display *x_dpy = XOpenDisplay(NULL);
                if (x_dpy) {
                    Window root;
                    int rx = 0, ry = 0;
                    unsigned int gw = 0, gh = 0, b = 0, d = 0;
                    if (XGetGeometry(x_dpy, (Window)g_embed_wid, &root, &rx, &ry, &gw, &gh, &b, &d) && gw > 0 && gh > 0) {
                        int win_w = 0, win_h = 0;
                        SDL_GetWindowSize(window, &win_w, &win_h);
                        if ((int)gw != win_w || (int)gh != win_h) {
                            SDL_SetWindowSize(window, (int)gw, (int)gh);
                            SDL_RenderSetViewport(renderer, NULL);
                        }
                    }
                    XCloseDisplay(x_dpy);
                }
            }
        }
        SDL_GetRendererOutputSize(renderer, &cur_w, &cur_h);
        if (cur_w <= 0 || cur_h <= 0) {
            cur_w = ctx.width > 0 ? (int)ctx.width : (int)init_w;
            cur_h = ctx.height > 0 ? (int)ctx.height : (int)init_h;
        }

        /* HUD dimensions */
        int hud_w = 660;
        int hud_h = 44;
        int hud_x = (cur_w - hud_w) / 2;
        int hud_y = cur_h - 58;

        /* Slider rect */
        SDL_Rect slider_rect = { hud_x + 64, hud_y + 16, 130, 10 };

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                g_running = 0;
            } else if (ev.type == SDL_MOUSEMOTION) {
                g_last_mouse_time = SDL_GetTicks();
                if (!g_streamer_mode) {
                    SDL_ShowCursor(SDL_ENABLE);
                } else {
                    SDL_ShowCursor(SDL_DISABLE);
                }

                if (dragging_volume && !g_streamer_mode) {
                    int mx = ev.motion.x;
                    float ratio = (float)(mx - slider_rect.x) / (float)slider_rect.w;
                    if (ratio < 0.0f) ratio = 0.0f;
                    if (ratio > 1.0f) ratio = 1.0f;
                    g_volume = ratio * 1.5f;
                    g_muted = false;
                    g_audio_dirty = 1;
                    g_volume_toast_time = SDL_GetTicks();
                }
            } else if (ev.type == SDL_MOUSEBUTTONDOWN) {
                g_last_mouse_time = SDL_GetTicks();
                if (!g_streamer_mode) {
                    SDL_ShowCursor(SDL_ENABLE);
                } else {
                    SDL_ShowCursor(SDL_DISABLE);
                }

                if (ev.button.button == SDL_BUTTON_LEFT) {
                    if (ev.button.clicks == 2 && g_embed_wid == 0) {
                        /* Double-click toggles fullscreen */
                        update_fullscreen(window, !g_fullscreen);
                    } else if (!g_streamer_mode && g_hud_visible && ev.button.y >= hud_y && ev.button.y <= hud_y + hud_h) {
                        int bx = ev.button.x;
                        /* Mute toggle button (hud_x + 10 .. + 55) */
                        if (bx >= hud_x + 10 && bx <= hud_x + 58) {
                            g_muted = !g_muted;
                            g_audio_dirty = 1;
                            g_volume_toast_time = SDL_GetTicks();
                        }
                        /* Volume slider (hud_x + 64 .. + 194) */
                        else if (bx >= slider_rect.x && bx <= slider_rect.x + slider_rect.w) {
                            dragging_volume = true;
                            float ratio = (float)(bx - slider_rect.x) / (float)slider_rect.w;
                            g_volume = ratio * 1.5f;
                            g_muted = false;
                            g_audio_dirty = 1;
                            g_volume_toast_time = SDL_GetTicks();
                        }
                        /* Stats toggle button (hud_x + 475 .. + 530) */
                        else if (bx >= hud_x + 475 && bx <= hud_x + 530) {
                            g_show_stats = !g_show_stats;
                        }
                        /* Fullscreen toggle button (hud_x + 535 .. + 590) */
                        else if (bx >= hud_x + 535 && bx <= hud_x + 590) {
                            if (g_embed_wid == 0) {
                                update_fullscreen(window, !g_fullscreen);
                            }
                        }
                        /* Exit button (hud_x + 595 .. + 645) - only active in standalone window */
                        else if (bx >= hud_x + 595 && bx <= hud_x + 645) {
                            if (g_embed_wid == 0) {
                                g_running = 0;
                            }
                        }
                    }
                }
            } else if (ev.type == SDL_MOUSEBUTTONUP) {
                if (ev.button.button == SDL_BUTTON_LEFT) {
                    dragging_volume = false;
                }
            } else if (ev.type == SDL_MOUSEWHEEL) {
                g_last_mouse_time = SDL_GetTicks();
                if (!g_streamer_mode) {
                    SDL_ShowCursor(SDL_ENABLE);
                }

                if (ev.wheel.y > 0) {
                    g_volume = (g_volume < 1.45f) ? g_volume + 0.05f : 1.5f;
                    g_muted = false;
                } else if (ev.wheel.y < 0) {
                    g_volume = (g_volume > 0.05f) ? g_volume - 0.05f : 0.0f;
                }
                g_audio_dirty = 1;
                if (!g_streamer_mode) {
                    g_volume_toast_time = SDL_GetTicks();
                }
            } else if (ev.type == SDL_KEYDOWN) {
                g_last_mouse_time = SDL_GetTicks();
                if (!g_streamer_mode) {
                    SDL_ShowCursor(SDL_ENABLE);
                }

                if ((ev.key.keysym.sym == SDLK_ESCAPE || ev.key.keysym.sym == SDLK_q) && g_embed_wid == 0) {
                    g_running = 0;
                } else if ((ev.key.keysym.sym == SDLK_f || ev.key.keysym.sym == SDLK_F11) && g_embed_wid == 0) {
                    update_fullscreen(window, !g_fullscreen);
                } else if (ev.key.keysym.sym == SDLK_s || ev.key.keysym.sym == SDLK_F10) {
                    g_streamer_mode = !g_streamer_mode;
                    if (g_streamer_mode) {
                        SDL_ShowCursor(SDL_DISABLE);
                        printf("[*] Streamer Mode (Clean Feed) AKTIVIERT: Overlays und Mauszeiger gesperrt\n");
                    } else {
                        g_last_mouse_time = SDL_GetTicks();
                        SDL_ShowCursor(SDL_ENABLE);
                        printf("[*] Streamer Mode DEAKTIVIERT: Normaler OSD-Modus aktiv\n");
                    }
                } else if (ev.key.keysym.sym == SDLK_m) {
                    g_muted = !g_muted;
                    g_audio_dirty = 1;
                    if (!g_streamer_mode) g_volume_toast_time = SDL_GetTicks();
                } else if (ev.key.keysym.sym == SDLK_0 || ev.key.keysym.sym == SDLK_UP || ev.key.keysym.sym == SDLK_PLUS) {
                    g_volume = (g_volume < 1.45f) ? g_volume + 0.05f : 1.5f;
                    g_muted = false;
                    g_audio_dirty = 1;
                    if (!g_streamer_mode) g_volume_toast_time = SDL_GetTicks();
                } else if (ev.key.keysym.sym == SDLK_9 || ev.key.keysym.sym == SDLK_DOWN || ev.key.keysym.sym == SDLK_MINUS) {
                    g_volume = (g_volume > 0.05f) ? g_volume - 0.05f : 0.0f;
                    g_audio_dirty = 1;
                    if (!g_streamer_mode) g_volume_toast_time = SDL_GetTicks();
                } else if (ev.key.keysym.sym == SDLK_TAB || ev.key.keysym.sym == SDLK_i) {
                    g_show_stats = !g_show_stats;
                } else if (ev.key.keysym.sym == SDLK_h) {
                    g_hdr_mode = (g_hdr_mode + 1) % 3;
                    if (g_hdr_mode == 0)
                        snprintf(g_toast_custom, sizeof(g_toast_custom), "HDR: AUTO (SDR MONITOR TONEMAP)");
                    else if (g_hdr_mode == 1)
                        snprintf(g_toast_custom, sizeof(g_toast_custom), "HDR: PASSTHROUGH (HDR MONITOR NATIVE)");
                    else
                        snprintf(g_toast_custom, sizeof(g_toast_custom), "HDR: OFF (SDR DIRECT BYPASS)");
                    g_toast_time = SDL_GetTicks();
                } else if (ev.key.keysym.sym == SDLK_RIGHTBRACKET) {
                    g_hdr_exposure = (g_hdr_exposure < 150.0f) ? g_hdr_exposure + 5.0f : 150.0f;
                    snprintf(g_toast_custom, sizeof(g_toast_custom), "HDR TONEMAP EXPOSURE: %.1f", g_hdr_exposure);
                    g_toast_time = SDL_GetTicks();
                } else if (ev.key.keysym.sym == SDLK_LEFTBRACKET) {
                    g_hdr_exposure = (g_hdr_exposure > 10.0f) ? g_hdr_exposure - 5.0f : 10.0f;
                    snprintf(g_toast_custom, sizeof(g_toast_custom), "HDR TONEMAP EXPOSURE: %.1f", g_hdr_exposure);
                    g_toast_time = SDL_GetTicks();
                } else if (ev.key.keysym.sym == SDLK_v) {
                    enable_vsync = !enable_vsync;
                    SDL_GL_SetSwapInterval(enable_vsync ? 1 : 0);
                    snprintf(g_toast_custom, sizeof(g_toast_custom), "VSYNC: %s", enable_vsync ? "ON (SMOOTH 120Hz/60Hz)" : "OFF (UNTHROTTLED)");
                    g_toast_time = SDL_GetTicks();
                } else if (ev.key.keysym.sym == SDLK_r) {
                    glob_t hg;
                    if (glob("/sys/bus/pci/drivers/open_gc573/*/hpd_pulse", 0, NULL, &hg) == 0 && hg.gl_pathc > 0) {
                        FILE *hf = fopen(hg.gl_pathv[0], "w");
                        if (hf) {
                            fputs("1\n", hf);
                            fclose(hf);
                            printf("[*] Hardware HDMI HPD re-sync pulse triggered\n");
                        }
                        globfree(&hg);
                    }
                    stream_stop(&ctx);
                    g_audio_resync_needed = 1;
                    snprintf(g_toast_custom, sizeof(g_toast_custom), "HDMI RE-SYNC");
                    g_toast_time = SDL_GetTicks();
                    last_retry_time = SDL_GetTicks() - 100;
                }
            } else if (ev.type == SDL_WINDOWEVENT) {
                if (ev.window.event == SDL_WINDOWEVENT_RESIZED ||
                    ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED ||
                    ev.window.event == SDL_WINDOWEVENT_EXPOSED ||
                    ev.window.event == SDL_WINDOWEVENT_MOVED) {
                    g_need_geom_check = 30;
                }
            }
        }

        /* If no active stream, attempt reconnect and render standby UI */
        if (!ctx.active) {
            if (now_ticks - last_retry_time >= 300) {
                last_retry_time = now_ticks;
                if (stream_start(&ctx, renderer)) {
                    printf("[+] HDMI signal detected and locked: %ux%u%c!\n", ctx.width, ctx.height, ctx.interlaced ? 'i' : 'p');
                    snprintf(g_toast_custom, sizeof(g_toast_custom), "SIGNAL LOCKED");
                    g_toast_time = SDL_GetTicks();
                    last_frame_ticks = SDL_GetTicks();
                }
            }

            if (!ctx.active) {
                SDL_SetRenderDrawColor(renderer, 13, 17, 23, 255);
                SDL_RenderClear(renderer);

                int card_w = 560;
                int card_h = 180;
                int card_x = (cur_w - card_w) / 2;
                int card_y = (cur_h - card_h) / 2 - 20;
                int center_x = card_x + card_w / 2;

                SDL_Rect card_rect = { card_x, card_y, card_w, card_h };
                SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
                /* Card body: sleek dark studio background */
                SDL_SetRenderDrawColor(renderer, 18, 22, 32, 245);
                SDL_RenderFillRect(renderer, &card_rect);
                /* Card outline: subtle dark slate border */
                SDL_SetRenderDrawColor(renderer, 35, 45, 65, 255);
                SDL_RenderDrawRect(renderer, &card_rect);

                /* Top accent line (2px royal blue) */
                SDL_Rect top_accent = { card_x + 1, card_y + 1, card_w - 2, 2 };
                SDL_SetRenderDrawColor(renderer, 59, 130, 246, 200);
                SDL_RenderFillRect(renderer, &top_accent);

                /* Texts - automatically, perfectly centered */
                draw_string_centered(renderer, "EVERLIVE GAMING SCREEN", center_x, card_y + 26, 2, (SDL_Color){248, 250, 252, 255});
                draw_string_centered(renderer, "Waiting for HDMI signal...", center_x, card_y + 72, 1, (SDL_Color){234, 179, 8, 255});
                draw_string_centered(renderer, "Power on console or video source", center_x, card_y + 100, 1, (SDL_Color){148, 163, 184, 255});
                draw_string_centered(renderer, "[H] Re-Sync   *   [S] Streamer Mode   *   [Esc] Exit", center_x, card_y + 134, 1, (SDL_Color){96, 165, 250, 255});

                /* Draw HUD so volume and controls remain usable in standby */
                if (g_hud_visible && !g_streamer_mode) {
                    SDL_Rect hud_bg = { hud_x, hud_y, hud_w, hud_h };
                    SDL_SetRenderDrawColor(renderer, 15, 18, 26, 215);
                    SDL_RenderFillRect(renderer, &hud_bg);
                    SDL_SetRenderDrawColor(renderer, 50, 65, 95, 230);
                    SDL_RenderDrawRect(renderer, &hud_bg);

                    SDL_Rect btn_vol = { hud_x + 10, hud_y + 10, 48, 24 };
                    SDL_SetRenderDrawColor(renderer, g_muted ? 180 : 35, g_muted ? 40 : 45, g_muted ? 40 : 60, 200);
                    SDL_RenderFillRect(renderer, &btn_vol);
                    draw_string(renderer, g_muted ? "MUTE" : "VOL", hud_x + 14, hud_y + 14, 1, (SDL_Color){255, 255, 255, 255});

                    SDL_SetRenderDrawColor(renderer, 35, 42, 56, 255);
                    SDL_RenderFillRect(renderer, &slider_rect);
                    int filled_w = (int)(slider_rect.w * (g_volume / 1.5f));
                    if (filled_w > slider_rect.w) filled_w = slider_rect.w;
                    if (filled_w > 0 && !g_muted) {
                        SDL_Rect fill_rect = { slider_rect.x, slider_rect.y, filled_w, slider_rect.h };
                        SDL_SetRenderDrawColor(renderer, 56, 139, 253, 255);
                        SDL_RenderFillRect(renderer, &fill_rect);
                    }
                    int handle_x = slider_rect.x + filled_w - 4;
                    if (handle_x < slider_rect.x) handle_x = slider_rect.x;
                    if (handle_x > slider_rect.x + slider_rect.w - 6) handle_x = slider_rect.x + slider_rect.w - 6;
                    SDL_Rect handle_rect = { handle_x, slider_rect.y - 3, 7, 16 };
                    SDL_SetRenderDrawColor(renderer, 240, 246, 252, 255);
                    SDL_RenderFillRect(renderer, &handle_rect);

                    char vol_txt[16];
                    if (g_muted) snprintf(vol_txt, sizeof(vol_txt), "OFF");
                    else snprintf(vol_txt, sizeof(vol_txt), "%3d%%", (int)(g_volume * 100.0f));
                    draw_string(renderer, vol_txt, hud_x + 202, hud_y + 18, 1, (SDL_Color){200, 215, 235, 255});

                    draw_string(renderer, "STANDBY", hud_x + 260, hud_y + 18, 1, (SDL_Color){210, 153, 34, 255});

                    SDL_Rect sep = { hud_x + 460, hud_y + 10, 1, 24 };
                    SDL_SetRenderDrawColor(renderer, 50, 65, 95, 180);
                    SDL_RenderFillRect(renderer, &sep);

                    SDL_Rect btn_full = { hud_x + 534, hud_y + 10, 54, 24 };
                    SDL_SetRenderDrawColor(renderer, 28, 35, 48, 200);
                    SDL_RenderFillRect(renderer, &btn_full);
                    draw_string(renderer, g_fullscreen ? "WNDW" : "FULL", hud_x + 539, hud_y + 18, 1, (SDL_Color){220, 230, 245, 255});

                    if (g_embed_wid == 0) {
                        SDL_Rect btn_exit = { hud_x + 596, hud_y + 10, 48, 24 };
                        SDL_SetRenderDrawColor(renderer, 150, 30, 30, 200);
                        SDL_RenderFillRect(renderer, &btn_exit);
                        draw_string(renderer, "EXIT", hud_x + 603, hud_y + 18, 1, (SDL_Color){255, 255, 255, 255});
                    }
                }

                if (now_ticks - last_title_time >= 1000) {
                    last_title_time = now_ticks;
                    if (g_embed_wid == 0) {
                        char title[180];
                        snprintf(title, sizeof(title), "EverLive - Waiting for HDMI signal... | Vol: %d%%",
                                 g_muted ? 0 : (int)(g_volume * 100.0f));
                        SDL_SetWindowTitle(window, title);
                    }
                }

                SDL_RenderPresent(renderer);
                SDL_Delay(16);
                continue;
            }
        }

        struct timespec t_poll_start, t_poll_end, t_dq_end, t_up_end, t_draw_end, t_pres_end;
        clock_gettime(CLOCK_MONOTONIC, &t_poll_start);
        int poll_ret = poll(&pfd, 1, 50);
        clock_gettime(CLOCK_MONOTONIC, &t_poll_end);
        uint32_t now_ticks_poll = SDL_GetTicks();
        uint32_t elapsed_since_frame = (now_ticks_poll >= last_frame_ticks) ? (now_ticks_poll - last_frame_ticks) : 0;

        if (poll_ret > 0 && (pfd.revents & POLLPRI)) {
            struct v4l2_event ev = {};
            while (ioctl(fd, VIDIOC_DQEVENT, &ev) == 0) {
                if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
                    struct v4l2_format chk_fmt = {};
                    chk_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
                    if (ioctl(fd, VIDIOC_G_FMT, &chk_fmt) == 0) {
                        /* Check if resolution or pixelformat genuinely changed */
                        if (chk_fmt.fmt.pix.width != ctx.width ||
                            chk_fmt.fmt.pix.height != ctx.height ||
                            chk_fmt.fmt.pix.pixelformat != ctx.pixelformat) {
                            printf("[*] Hardware resolution change: %ux%u -> %ux%u. Re-synchronizing stream...\n",
                                   ctx.width, ctx.height, chk_fmt.fmt.pix.width, chk_fmt.fmt.pix.height);
                            stream_stop(&ctx);
                            g_audio_resync_needed = 1;
                        } else {
                            /* Format/resolution unchanged: dynamically update HDR/color properties without dropping any frames! */
                            ctx.bytesperline = chk_fmt.fmt.pix.bytesperline;
                            ctx.is_full_range = (chk_fmt.fmt.pix.quantization == V4L2_QUANTIZATION_FULL_RANGE);
                            ctx.is_hdr = (chk_fmt.fmt.pix.colorspace == V4L2_COLORSPACE_BT2020) ||
                                         (chk_fmt.fmt.pix.xfer_func == V4L2_XFER_FUNC_SMPTE2084);
                            ctx.is_hlg = (chk_fmt.fmt.pix.xfer_func == 10);
                            g_audio_resync_needed = 1;
                        }
                    }
                }
            }
        }

        if (poll_ret <= 0) {
            if (ctx.active && elapsed_since_frame > 2500) {
                printf("[*] HDMI Frame-Stall detected (%u ms without frame), switching to standby...\n",
                       elapsed_since_frame);
                stream_stop(&ctx);
            } else if (poll_ret > 0 && (pfd.revents & (POLLERR | POLLHUP))) {
                printf("[*] HDMI signal disconnected, switching to standby...\n");
                stream_stop(&ctx);
            }
            continue;
        }

        struct v4l2_buffer buf = {};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0) {
            if (errno == EAGAIN || errno == EINTR) {
                if (ctx.active && elapsed_since_frame > 2500) {
                    printf("[*] HDMI Frame-Stall (EAGAIN/EINTR > 2500 ms), switching to standby...\n");
                    stream_stop(&ctx);
                }
                continue;
            }
            printf("[*] Video buffer invalid (%s), switching to standby...\n", strerror(errno));
            stream_stop(&ctx);
            continue;
        }
        clock_gettime(CLOCK_MONOTONIC, &t_dq_end);

        last_frame_ticks = now_ticks_poll;

        /* Calculate exact capture-to-display latency */
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);
        uint64_t now_us = (uint64_t)now_ts.tv_sec * 1000000ULL + now_ts.tv_nsec / 1000ULL;
        uint64_t buf_time_us = (uint64_t)buf.timestamp.tv_sec * 1000000ULL + buf.timestamp.tv_usec;
        int64_t lat_us = (int64_t)(now_us - buf_time_us);
        if (lat_us < 0) lat_us = 0;

        /* Low-Latency Backlog Catch-Up:
         * Trigger catch-up if latency exceeds > 2 full frame periods (> 18 ms at 120Hz or > 32 ms at 60Hz).
         * At most ONE frame is dropped per iteration so latency decays gradually and the
         * presentation cadence (VSync) stays uniform - a while-loop burst drain of whole frame
         * batches here causes single huge skips/judder every time the queue fills up. */
        uint64_t stale_thresh_us = (live_fps > 90.0) ? 18000 : 32000;
        if ((uint64_t)lat_us > stale_thresh_us) {
            struct v4l2_buffer next_buf = {};
            next_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            next_buf.memory = V4L2_MEMORY_MMAP;
            if (ioctl(fd, VIDIOC_DQBUF, &next_buf) == 0) {
                ioctl(fd, VIDIOC_QBUF, &buf);
                buf = next_buf;
                buf_time_us = (uint64_t)buf.timestamp.tv_sec * 1000000ULL + buf.timestamp.tv_usec;
                lat_us = (int64_t)(now_us - buf_time_us);
                if (lat_us < 0) lat_us = 0;
            }
        }
        lat_ms = (double)lat_us / 1000.0;

        /* Direct GPU texture upload from DMA buffer (< 0.4 ms) */
        if (ctx.gl_ready) {
            if (ctx.pixelformat == V4L2_PIX_FMT_YUYV) {
                glBindTexture(GL_TEXTURE_2D, ctx.gl_tex_y);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, (int)(ctx.bytesperline / 4));
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)(ctx.width / 2), (int)ctx.height,
                                GL_RGBA, GL_UNSIGNED_BYTE, ctx.buffers[buf.index].start);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            } else if (ctx.pixelformat == V4L2_PIX_FMT_NV12) {
                const uint8_t *pixels = (const uint8_t *)ctx.buffers[buf.index].start;
                glBindTexture(GL_TEXTURE_2D, ctx.gl_tex_y);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, (int)ctx.bytesperline);
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)ctx.width, (int)ctx.height,
                                GL_RED, GL_UNSIGNED_BYTE, pixels);
                glBindTexture(GL_TEXTURE_2D, ctx.gl_tex_uv);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, (int)(ctx.bytesperline / 2));
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)(ctx.width / 2), (int)(ctx.height / 2),
                                GL_RG, GL_UNSIGNED_BYTE, pixels + (size_t)ctx.bytesperline * ctx.height);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            } else if (ctx.pixelformat == V4L2_PIX_FMT_BGR24) {
                glBindTexture(GL_TEXTURE_2D, ctx.gl_tex_y);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, (int)(ctx.bytesperline / 3));
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)ctx.width, (int)ctx.height,
                                GL_BGR, GL_UNSIGNED_BYTE, ctx.buffers[buf.index].start);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            } else if (ctx.pixelformat == V4L2_PIX_FMT_RGB32) {
                glBindTexture(GL_TEXTURE_2D, ctx.gl_tex_y);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, (int)(ctx.bytesperline / 4));
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (int)ctx.width, (int)ctx.height,
                                GL_BGRA, GL_UNSIGNED_BYTE, ctx.buffers[buf.index].start);
                glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            }
        } else {
            if (ctx.sdl_format == SDL_PIXELFORMAT_NV12) {
                const Uint8 *pixels = (const Uint8 *)ctx.buffers[buf.index].start;
                SDL_UpdateNVTexture(ctx.texture, NULL,
                                    pixels, (int)ctx.bytesperline,
                                    pixels + (size_t)ctx.bytesperline * ctx.height, (int)ctx.bytesperline);
            } else {
                SDL_UpdateTexture(ctx.texture, NULL, ctx.buffers[buf.index].start, (int)ctx.bytesperline);
            }
        }

        /* Immediately return buffer to driver DMA queue so next frame can capture without starvation */
        ioctl(fd, VIDIOC_QBUF, &buf);
        clock_gettime(CLOCK_MONOTONIC, &t_up_end);

        /* Aspect-ratio preserving letterboxed scaling */
        SDL_Rect dest_rect;
        if (ctx.width > 0 && ctx.height > 0 && cur_w > 0 && cur_h > 0) {
            /* 3840x1080 is an anamorphic half-vertical 4K120 signal (e.g. RetroTINK 4K Pro)
             * that requires 2x vertical stretching to restore the intended 16:9 aspect ratio.
             * 1440x480 / 1440x576 are 2x pixel-repeated SD signals (CEA-861 VIC 6/21)
             * 2880x240 is 4x/8x pixel-repeated 240p (RetroTINK 4K).
             * 720x400 / 640x400 are classic DOS VGA modes displayed on 4:3 CRT monitors.
             * These require 4:3 aspect ratio display (effective width = 720 or 320 or 533). */
            int eff_w = ctx.width;
            int eff_h = ctx.height;
            if (ctx.width == 3840 && ctx.height == 1080) {
                eff_h = ctx.height * 2;
            } else if (ctx.width == 1440 && (ctx.height == 480 || ctx.height == 576)) {
                eff_w = 720;
            } else if (ctx.width == 2880 && ctx.height == 240) {
                eff_w = 320;
            } else if (ctx.width == 1440 && ctx.height == 240) {
                eff_w = 320;
            } else if (ctx.width == 1920 && ctx.height == 240) {
                eff_w = 320;
            } else if ((ctx.width == 720 || ctx.width == 640) && ctx.height == 400) {
                eff_w = (ctx.height * 4) / 3;
            }
            double scale_w = (double)cur_w / (double)eff_w;
            double scale_h = (double)cur_h / (double)eff_h;
            double scale = (scale_w < scale_h) ? scale_w : scale_h;
            dest_rect.w = (int)(eff_w * scale);
            dest_rect.h = (int)(eff_h * scale);
            dest_rect.x = (cur_w - dest_rect.w) / 2;
            dest_rect.y = (cur_h - dest_rect.h) / 2;
        } else {
            dest_rect.x = 0;
            dest_rect.y = 0;
            dest_rect.w = cur_w;
            dest_rect.h = cur_h;
        }

        static int last_dest_w = 0, last_dest_h = 0;
        if (dest_rect.w != last_dest_w || dest_rect.h != last_dest_h) {
            glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            last_dest_w = dest_rect.w;
            last_dest_h = dest_rect.h;
        }

        bool active_hdr = (g_hdr_mode == 1) || (g_hdr_mode == 0 && ctx.is_hdr);
        if (ctx.gl_ready) {
            GLint prev_prog = 0;
            GLint prev_array_buf = 0;
            glGetIntegerv(GL_CURRENT_PROGRAM, &prev_prog);
            glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &prev_array_buf);

            glDisable(GL_DEPTH_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_BLEND);

            glUseProgram(ctx.gl_prog);

            int gl_y = cur_h - (dest_rect.y + dest_rect.h);
            glViewport(dest_rect.x, gl_y, dest_rect.w, dest_rect.h);

            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, ctx.gl_tex_y);
            glUniform1i(ctx.u_loc_tex_y, 0);

            if (ctx.pixelformat == V4L2_PIX_FMT_NV12) {
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, ctx.gl_tex_uv);
                glUniform1i(ctx.u_loc_tex_uv, 1);
            }

            glUniform2f(ctx.u_loc_resolution, (float)ctx.width, (float)ctx.height);
            glUniform1i(ctx.u_loc_is_hdr, active_hdr ? 1 : 0);
            glUniform1i(ctx.u_loc_is_hlg, (active_hdr && ctx.is_hlg) ? 1 : 0);
            glUniform1i(ctx.u_loc_is_full_range, ctx.is_full_range ? 1 : 0);
            glUniform1f(ctx.u_loc_exposure, g_hdr_exposure);
            glUniform1i(ctx.u_loc_tonemap_mode, (g_hdr_mode == 1) ? 1 : 0);

            int fmt_type = 0;
            if (ctx.pixelformat == V4L2_PIX_FMT_NV12) fmt_type = 1;
            else if (ctx.pixelformat == V4L2_PIX_FMT_BGR24 || ctx.pixelformat == V4L2_PIX_FMT_RGB32) fmt_type = 2;
            glUniform1i(ctx.u_loc_fmt_type, fmt_type);

            static const float quad_verts[] = {
                -1.0f, -1.0f,  0.0f, 1.0f,
                 1.0f, -1.0f,  1.0f, 1.0f,
                -1.0f,  1.0f,  0.0f, 0.0f,
                 1.0f,  1.0f,  1.0f, 0.0f,
            };

            glBindBuffer(GL_ARRAY_BUFFER, 0);
            glEnableVertexAttribArray(0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), &quad_verts[0]);
            glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), &quad_verts[2]);

            glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

            glDisableVertexAttribArray(0);
            glDisableVertexAttribArray(1);

            // Restore state for SDL2 2D HUD rendering
            glBindBuffer(GL_ARRAY_BUFFER, prev_array_buf);
            glUseProgram(prev_prog);
            glViewport(0, 0, cur_w, cur_h);
        } else {
            SDL_RenderCopy(renderer, ctx.texture, NULL, &dest_rect);
        }

        frame_count++;

        static uint64_t last_now_us = 0;
        uint64_t frame_delta_us = last_now_us ? (now_us - last_now_us) : 8333;
        last_now_us = now_us;

        /* Expected interval: 8333 us (120 Hz) or 16666 us (60 Hz) */
        uint64_t expected = (frame_delta_us < 12500) ? 8333 : 16666;
        int64_t diff = (int64_t)frame_delta_us - (int64_t)expected;
        if (diff < 0) diff = -diff;
        /* Count jitter on genuine delivery stalls (> 5.5 ms deviation) */
        if (diff > 5500) {
            jitter_count++;
        }

        now_ticks = SDL_GetTicks();

        /* Auto-hide HUD and cursor on idle */
        if (g_streamer_mode) {
            g_hud_visible = false;
            SDL_ShowCursor(SDL_DISABLE);
        } else if (now_ticks - g_last_mouse_time > 2500 && !dragging_volume) {
            g_hud_visible = false;
            SDL_ShowCursor(SDL_DISABLE);
        } else {
            g_hud_visible = true;
        }

        /* 1. Draw On-Screen Display HUD if active */
        if (g_hud_visible && !g_streamer_mode) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

            /* Main HUD Panel Bar */
            SDL_Rect hud_bg = { hud_x, hud_y, hud_w, hud_h };
            SDL_SetRenderDrawColor(renderer, 15, 18, 26, 215);
            SDL_RenderFillRect(renderer, &hud_bg);
            SDL_SetRenderDrawColor(renderer, 50, 65, 95, 230);
            SDL_RenderDrawRect(renderer, &hud_bg);

            /* Mute / Vol Button */
            SDL_Rect btn_vol = { hud_x + 10, hud_y + 10, 48, 24 };
            SDL_SetRenderDrawColor(renderer, g_muted ? 180 : 35, g_muted ? 40 : 45, g_muted ? 40 : 60, 200);
            SDL_RenderFillRect(renderer, &btn_vol);
            draw_string(renderer, g_muted ? "MUTE" : "VOL", hud_x + 14, hud_y + 14, 1, (SDL_Color){255, 255, 255, 255});

            /* Volume Slider Track */
            SDL_SetRenderDrawColor(renderer, 35, 42, 56, 255);
            SDL_RenderFillRect(renderer, &slider_rect);

            /* Volume Filled Bar */
            int filled_w = (int)(slider_rect.w * (g_volume / 1.5f));
            if (filled_w > slider_rect.w) filled_w = slider_rect.w;
            if (filled_w > 0 && !g_muted) {
                SDL_Rect fill_rect = { slider_rect.x, slider_rect.y, filled_w, slider_rect.h };
                SDL_SetRenderDrawColor(renderer, 56, 139, 253, 255);
                SDL_RenderFillRect(renderer, &fill_rect);
            }

            /* Volume Slider Handle */
            int handle_x = slider_rect.x + filled_w - 4;
            if (handle_x < slider_rect.x) handle_x = slider_rect.x;
            if (handle_x > slider_rect.x + slider_rect.w - 6) handle_x = slider_rect.x + slider_rect.w - 6;
            SDL_Rect handle_rect = { handle_x, slider_rect.y - 3, 7, 16 };
            SDL_SetRenderDrawColor(renderer, 240, 246, 252, 255);
            SDL_RenderFillRect(renderer, &handle_rect);

            /* Volume Text (Percentage) */
            char vol_txt[16];
            if (g_muted)
                snprintf(vol_txt, sizeof(vol_txt), "OFF");
            else
                snprintf(vol_txt, sizeof(vol_txt), "%3d%%", (int)(g_volume * 100.0f));
            draw_string(renderer, vol_txt, hud_x + 202, hud_y + 18, 1, (SDL_Color){200, 215, 235, 255});

            /* Signal Info Badge */
            char sig_txt[48];
            snprintf(sig_txt, sizeof(sig_txt), "%ux%u%c@%.2f %s", ctx.width, ctx.height, ctx.interlaced ? 'i' : 'p', live_fps > 0.0 ? live_fps : 60.0, get_chroma_desc(ctx.pixelformat));
            draw_string(renderer, sig_txt, hud_x + 250, hud_y + 18, 1, (SDL_Color){63, 185, 80, 255});

            /* Separator */
            SDL_Rect sep = { hud_x + 460, hud_y + 10, 1, 24 };
            SDL_SetRenderDrawColor(renderer, 50, 65, 95, 180);
            SDL_RenderFillRect(renderer, &sep);

            /* Stats Button */
            SDL_Rect btn_stats = { hud_x + 472, hud_y + 10, 54, 24 };
            SDL_SetRenderDrawColor(renderer, g_show_stats ? 40 : 25, g_show_stats ? 60 : 32, g_show_stats ? 80 : 45, 200);
            SDL_RenderFillRect(renderer, &btn_stats);
            draw_string(renderer, "STATS", hud_x + 477, hud_y + 18, 1, (SDL_Color){220, 230, 245, 255});

            /* Fullscreen Button */
            SDL_Rect btn_full = { hud_x + 534, hud_y + 10, 54, 24 };
            SDL_SetRenderDrawColor(renderer, 28, 35, 48, 200);
            SDL_RenderFillRect(renderer, &btn_full);
            draw_string(renderer, g_fullscreen ? "WNDW" : "FULL", hud_x + 539, hud_y + 18, 1, (SDL_Color){220, 230, 245, 255});

            /* Exit Button (only in standalone window) */
            if (g_embed_wid == 0) {
                SDL_Rect btn_exit = { hud_x + 596, hud_y + 10, 48, 24 };
                SDL_SetRenderDrawColor(renderer, 150, 30, 30, 200);
                SDL_RenderFillRect(renderer, &btn_exit);
                draw_string(renderer, "EXIT", hud_x + 603, hud_y + 18, 1, (SDL_Color){255, 255, 255, 255});
            }
        }

        /* 2. Top-Right Stats HUD Overlay (if enabled) */
        if (g_show_stats && !g_streamer_mode) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            int box_w = 200;
            bool active_hdr_badge = active_hdr || ctx.is_hdr;
            int box_h = active_hdr_badge ? 95 : 75;
            int box_x = cur_w - box_w - 20;
            int box_y = 20;

            SDL_Rect stats_bg = { box_x, box_y, box_w, box_h };
            SDL_SetRenderDrawColor(renderer, 12, 15, 22, 200);
            SDL_RenderFillRect(renderer, &stats_bg);
            SDL_SetRenderDrawColor(renderer, 45, 55, 80, 220);
            SDL_RenderDrawRect(renderer, &stats_bg);

            char line1[32], line2[32], line3[32], line4[32];
            snprintf(line1, sizeof(line1), "FPS: %.2f (%.2fms)", live_fps > 0.0 ? live_fps : 60.0, live_fps > 0.0 ? (1000.0 / live_fps) : 16.67);
            snprintf(line2, sizeof(line2), "LAG: %.1f ms", lat_ms);
            snprintf(line3, sizeof(line3), "JITTER: %u/s", live_jitter);
            if (active_hdr) {
                if (g_hdr_mode == 1) {
                    snprintf(line4, sizeof(line4), "HDR: %s (Passthrough)", ctx.is_hlg ? "HLG" : "PQ");
                } else {
                    snprintf(line4, sizeof(line4), "HDR: %s (Tonemap)", ctx.is_hlg ? "HLG" : "PQ");
                }
            } else if (ctx.is_hdr) {
                snprintf(line4, sizeof(line4), "HDR: Off (SDR Bypass)");
            }

            draw_string(renderer, line1, box_x + 12, box_y + 12, 1, (SDL_Color){63, 185, 80, 255});
            draw_string(renderer, line2, box_x + 12, box_y + 32, 1, (SDL_Color){88, 166, 255, 255});
            draw_string(renderer, line3, box_x + 12, box_y + 52, 1, (live_jitter > 5) ? (SDL_Color){248, 81, 73, 255} : (SDL_Color){139, 148, 158, 255});
            if (active_hdr) {
                draw_string(renderer, line4, box_x + 12, box_y + 72, 1, (SDL_Color){255, 215, 0, 255});
            } else if (ctx.is_hdr) {
                draw_string(renderer, line4, box_x + 12, box_y + 72, 1, (SDL_Color){160, 175, 195, 255});
            }
        }

        /* 3. Center Toast (pops up on scroll/key volume changes or HDMI re-sync) */
        if (!g_streamer_mode && (now_ticks - g_volume_toast_time < 1200 || now_ticks - g_toast_time < 1200)) {
            SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
            int tw = 220;
            int th = 40;
            int tx = (cur_w - tw) / 2;
            int ty = 40;

            SDL_Rect t_rect = { tx, ty, tw, th };
            SDL_SetRenderDrawColor(renderer, 15, 18, 26, 230);
            SDL_RenderFillRect(renderer, &t_rect);
            SDL_SetRenderDrawColor(renderer, 56, 139, 253, 240);
            SDL_RenderDrawRect(renderer, &t_rect);

            char toast_msg[64];
            if (now_ticks - g_toast_time < 1200 && g_toast_custom[0]) {
                snprintf(toast_msg, sizeof(toast_msg), "%s", g_toast_custom);
            } else if (g_muted) {
                snprintf(toast_msg, sizeof(toast_msg), "AUDIO: MUTED");
            } else {
                snprintf(toast_msg, sizeof(toast_msg), "VOLUME: %3d%%", (int)(g_volume * 100.0f));
            }

            draw_string(renderer, toast_msg, tx + 20, ty + 16, 1, (SDL_Color){240, 246, 252, 255});
        }

        clock_gettime(CLOCK_MONOTONIC, &t_draw_end);
        SDL_RenderPresent(renderer);
        clock_gettime(CLOCK_MONOTONIC, &t_pres_end);

        sum_poll_us += diff_timespec_us(&t_poll_end, &t_poll_start);
        sum_dqbuf_us += diff_timespec_us(&t_dq_end, &t_poll_end);
        sum_upload_us += diff_timespec_us(&t_up_end, &t_dq_end);
        sum_draw_us += diff_timespec_us(&t_draw_end, &t_up_end);
        sum_present_us += diff_timespec_us(&t_pres_end, &t_draw_end);
        perf_samples++;

        if (now_ticks - last_title_time >= 1000) {
            uint32_t elapsed_ms = now_ticks - last_title_time;
            live_fps = (double)frame_count * 1000.0 / (double)elapsed_ms;
            live_jitter = jitter_count;
            double avg_poll = perf_samples ? (double)sum_poll_us / perf_samples / 1000.0 : 0.0;
            double avg_dqbuf = perf_samples ? (double)sum_dqbuf_us / perf_samples / 1000.0 : 0.0;
            double avg_upload = perf_samples ? (double)sum_upload_us / perf_samples / 1000.0 : 0.0;
            double avg_draw = perf_samples ? (double)sum_draw_us / perf_samples / 1000.0 : 0.0;
            double avg_present = perf_samples ? (double)sum_present_us / perf_samples / 1000.0 : 0.0;
            frame_count = 0;
            jitter_count = 0;
            sum_poll_us = sum_dqbuf_us = sum_upload_us = sum_draw_us = sum_present_us = 0;
            perf_samples = 0;
            last_title_time = now_ticks;

            char title[180];
            snprintf(title, sizeof(title), "EverLive - %ux%u%c @ %.2f FPS [%s%s] | Latency: %.1f ms | Vol: %d%%%s",
                     ctx.width, ctx.height, ctx.interlaced ? 'i' : 'p', live_fps,
                     get_chroma_desc(ctx.pixelformat),
                     ctx.is_hdr ? (ctx.is_hlg ? " HLG" : " HDR10") : "",
                     lat_ms, g_muted ? 0 : (int)(g_volume * 100.0f),
                     g_streamer_mode ? " [STREAMER MODE]" : "");
            if (g_embed_wid == 0) {
                SDL_SetWindowTitle(window, title);
            }
            printf("[*] Stream: %ux%u%c @ %.2f FPS [%s%s] | Latency: %.1f ms | Jitter: %u/s | Perf(ms): Poll=%.2f DQ=%.2f Up=%.2f Draw=%.2f Pres=%.2f%s\n",
                   ctx.width, ctx.height, ctx.interlaced ? 'i' : 'p', live_fps,
                   get_chroma_desc(ctx.pixelformat),
                   ctx.is_hdr ? (ctx.is_hlg ? " HLG" : " HDR10") : "",
                   lat_ms, live_jitter,
                   avg_poll, avg_dqbuf, avg_upload, avg_draw, avg_present,
                   g_streamer_mode ? " [STREAMER MODE]" : "");
            fflush(stdout);
        }
    }

    /* Cleanup */
    if (enable_audio) {
        pthread_join(audio_tid, NULL);
    }

    stream_stop(&ctx);
    close(fd);

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();

    printf("[+] Exited.\n");
    return 0;
}
