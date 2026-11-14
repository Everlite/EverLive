// SPDX-License-Identifier: GPL-2.0-only
/*
 * gc575_userd.c — experimental GC575 UVC capture engine for EverLive
 *
 * This daemon owns no PCIe function and claims no USB interface.  It uses the
 * kernel's uvcvideo node, negotiates a normal V4L2 format, converts frames to
 * the YUY2 framestore contract and publishes them for the EverLive preload
 * DSO (/dev/video64).
 *
 * It is intentionally conservative:
 *   - no descriptor burns, no VFIO, no raw usbfs by default
 *   - AT profile selection is optional (--profile 0|1)
 *   - MJPEG is decoded only when built with libavcodec
 */

#include "gc575_camera.h"
#include "gc575_v4l2.h"
#include "gc575_xu.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_running = 1;

static void sig_handler(int sig)
{
	(void)sig;
	g_running = 0;
}

static uint64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000U +
	       (uint64_t)(ts.tv_nsec / 1000000L);
}

static const char *fourcc_name(uint32_t fourcc, char out[5])
{
	out[0] = (char)(fourcc & 0xff);
	out[1] = (char)((fourcc >> 8) & 0xff);
	out[2] = (char)((fourcc >> 16) & 0xff);
	out[3] = (char)((fourcc >> 24) & 0xff);
	out[4] = '\0';
	return out;
}

static uint32_t parse_format(const char *name)
{
	if (!name || !strcasecmp(name, "auto"))
		return 0;
	if (!strcasecmp(name, "nv12"))
		return V4L2_PIX_FMT_NV12;
	if (!strcasecmp(name, "yuyv") || !strcasecmp(name, "yuy2"))
		return V4L2_PIX_FMT_YUYV;
	if (!strcasecmp(name, "mjpeg") || !strcasecmp(name, "mjpg"))
		return V4L2_PIX_FMT_MJPEG;
	return 0;
}

static void usage(const char *prog)
{
	printf("Usage: %s [options]\n\n", prog);
	printf("  -d, --device PATH     V4L2 node (default: auto-detect 07ca:0575)\n");
	printf("  -f, --format FMT      nv12 | yuyv | mjpeg | auto (default: nv12)\n");
	printf("  -s, --size WxH        requested size (default: 1920x1080)\n");
	printf("  -r, --fps N           requested frame rate (default: 240)\n");
	printf("  -p, --profile 0|1     select USB descriptor profile before capture\n");
	printf("  -l, --list            list modes exposed by the UVC node and exit\n");
	printf("  -n, --no-camera       do not create the /gc575_v4l2_shm framestore\n");
	printf("  -N, --frames N        stop after N published frames (0 = unlimited)\n");
	printf("  -t, --timeout MS      dequeue timeout in milliseconds (default: 1000)\n");
	printf("  -v, --verbose         verbose per-frame logging\n");
	printf("  -h, --help            show this help\n");
}

int main(int argc, char **argv)
{
	const char *device = NULL;
	const char *format_name = "nv12";
	unsigned int want_width = 1920;
	unsigned int want_height = 1080;
	unsigned int want_fps = 240;
	int profile = -1;
	bool list_only = false;
	bool no_camera = false;
	bool verbose = false;
	unsigned long frame_limit = 0;
	int timeout_ms = 1000;
	struct gc575_capture cap;
	char node[64] = { 0 };
	uint8_t *yuy2 = NULL;
	size_t yuy2_size;
	uint64_t frames = 0;
	uint64_t errors = 0;
	uint64_t last_stats = 0;
	uint64_t last_frames = 0;
	int ret;
	char cc[5];

	static const struct option long_opts[] = {
		{ "device",    required_argument, NULL, 'd' },
		{ "format",    required_argument, NULL, 'f' },
		{ "size",      required_argument, NULL, 's' },
		{ "fps",       required_argument, NULL, 'r' },
		{ "profile",   required_argument, NULL, 'p' },
		{ "list",      no_argument,       NULL, 'l' },
		{ "no-camera", no_argument,       NULL, 'n' },
		{ "frames",    required_argument, NULL, 'N' },
		{ "timeout",   required_argument, NULL, 't' },
		{ "verbose",   no_argument,       NULL, 'v' },
		{ "help",      no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "d:f:s:r:p:lnN:t:vh",
				  long_opts, NULL)) != -1) {
		switch (opt) {
		case 'd':
			device = optarg;
			break;
		case 'f':
			format_name = optarg;
			break;
		case 's':
			if (sscanf(optarg, "%ux%u", &want_width, &want_height) != 2) {
				fprintf(stderr, "bad size: %s\n", optarg);
				return 2;
			}
			break;
		case 'r':
			want_fps = (unsigned int)strtoul(optarg, NULL, 0);
			break;
		case 'p':
			profile = (int)strtol(optarg, NULL, 0);
			if (profile < 0 || profile > 1) {
				fprintf(stderr, "profile must be 0 or 1\n");
				return 2;
			}
			break;
		case 'l':
			list_only = true;
			break;
		case 'n':
			no_camera = true;
			break;
		case 'N':
			frame_limit = strtoul(optarg, NULL, 0);
			break;
		case 't':
			timeout_ms = (int)strtol(optarg, NULL, 0);
			break;
		case 'v':
			verbose = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}

	if (device) {
		snprintf(node, sizeof(node), "%s", device);
	} else {
		ret = gc575_find_video_node(node, sizeof(node));
		if (ret < 0) {
			fprintf(stderr,
				"[gc575] no GC575 UVC capture node found (%s)\n",
				strerror(-ret));
			return 1;
		}
	}

	if (list_only) {
		ret = gc575_list_modes(node);
		if (ret < 0)
			fprintf(stderr, "[gc575] list failed: %s\n", strerror(-ret));
		return ret < 0 ? 1 : 0;
	}

	/*
	 * Profile selection is a runtime AT command, not a flash.  Open a
	 * temporary node handle, send AT b0, then give the device a moment to
	 * re-enumerate before opening the capture path.
	 */
	if (profile >= 0) {
		int fd = open(node, O_RDWR | O_NONBLOCK);
		if (fd >= 0) {
			ret = gc575_xu_set_profile(fd, GC575_XU_UNIT,
						   (uint32_t)profile);
			close(fd);
			if (ret < 0)
				fprintf(stderr,
					"[gc575] AT b0 profile %d failed: %s\n",
					profile, strerror(-ret));
			else
				printf("[gc575] requested descriptor profile %d\n",
				       profile);
			usleep(500000);
			if (!device) {
				char again[64];
				if (gc575_find_video_node(again, sizeof(again)) == 0 &&
				    strcmp(again, node) != 0) {
					snprintf(node, sizeof(node), "%s", again);
					printf("[gc575] node moved to %s\n", node);
				}
			}
		} else {
			fprintf(stderr, "[gc575] open %s: %s\n", node,
				strerror(errno));
			return 1;
		}
	}

	uint32_t want_fmt = parse_format(format_name);
	if (want_fmt == 0 && strcasecmp(format_name, "auto") != 0) {
		fprintf(stderr, "[gc575] unknown format: %s\n", format_name);
		return 2;
	}
	if (!strcasecmp(format_name, "auto")) {
		want_width = want_height = 0;
		want_fps = 0;
	}

	memset(&cap, 0, sizeof(cap));
	cap.fd = -1;
	ret = gc575_capture_open(&cap, node, want_fmt, want_width, want_height,
				 want_fps);
	if (ret < 0) {
		fprintf(stderr, "[gc575] capture open failed: %s\n",
			strerror(-ret));
		return 1;
	}

	printf("[gc575] %s: %ux%u %s @ %u fps (driver interval)\n",
	       node, cap.width, cap.height,
	       fourcc_name(cap.pixelformat, cc), cap.fps);

	yuy2_size = (size_t)cap.width * cap.height * 2;
	yuy2 = malloc(yuy2_size);
	if (!yuy2) {
		fprintf(stderr, "[gc575] out of memory\n");
		gc575_capture_close(&cap);
		return 1;
	}

	if (!no_camera) {
		ret = gc575_camera_start(node);
		if (ret < 0)
			fprintf(stderr,
				"[gc575] framestore disabled: %s\n",
				strerror(-ret));
	}

	ret = gc575_capture_start(&cap);
	if (ret < 0) {
		fprintf(stderr, "[gc575] stream start failed: %s\n",
			strerror(-ret));
		free(yuy2);
		if (!no_camera)
			gc575_camera_stop();
		gc575_capture_close(&cap);
		return 1;
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	printf("[gc575] streaming; Ctrl-C to stop\n");
	last_stats = now_ms();
	last_frames = 0;

	while (g_running) {
		struct v4l2_buffer buf;
		size_t bytes = 0;

		ret = gc575_capture_dq(&cap, &buf, timeout_ms);
		if (ret == -ETIMEDOUT) {
			if (!no_camera)
				gc575_camera_mark_no_signal();
			if (verbose)
				printf("[gc575] no frame within %d ms\n", timeout_ms);
			continue;
		}
		if (ret == -EINTR || ret == -EAGAIN)
			continue;
		if (ret < 0) {
			fprintf(stderr, "[gc575] dequeue failed: %s\n",
				strerror(-ret));
			errors++;
			break;
		}

		if (buf.flags & V4L2_BUF_FLAG_ERROR) {
			errors++;
			gc575_capture_q(&cap, &buf);
			continue;
		}

		ret = gc575_convert_to_yuy2(&cap, &buf, yuy2, yuy2_size, &bytes);
		if (ret == 0) {
			if (!no_camera &&
			    !gc575_camera_publish(yuy2, bytes, cap.width,
						  cap.height, false))
				errors++;
			frames++;
		} else if (ret != -EAGAIN) {
			errors++;
			if (verbose)
				fprintf(stderr,
					"[gc575] convert %s failed: %s\n",
					fourcc_name(cap.pixelformat, cc),
					strerror(-ret));
		}

		gc575_capture_q(&cap, &buf);

		uint64_t now = now_ms();
		if (verbose && frames % 120 == 0)
			printf("[gc575] %llu frames, %llu errors\n",
			       (unsigned long long)frames,
			       (unsigned long long)errors);

		if (now - last_stats >= 2000) {
			double fps = (double)(frames - last_frames) * 1000.0 /
				     (double)(now - last_stats);
			printf("[gc575] %ux%u %s: %.2f fps, %llu frames, "
			       "%llu errors\n",
			       cap.width, cap.height,
			       fourcc_name(cap.pixelformat, cc), fps,
			       (unsigned long long)frames,
			       (unsigned long long)errors);
			last_stats = now;
			last_frames = frames;
		}

		if (frame_limit && frames >= frame_limit)
			break;
	}

	gc575_capture_stop(&cap);
	if (!no_camera) {
		gc575_camera_mark_no_signal();
		gc575_camera_stop();
	}
	gc575_capture_close(&cap);
	free(yuy2);

	printf("[gc575] stopped after %llu frames (%llu errors)\n",
	       (unsigned long long)frames, (unsigned long long)errors);
	return 0;
}
