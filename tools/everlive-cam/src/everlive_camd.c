// SPDX-License-Identifier: GPL-2.0-only
/*
 * everlive_camd.c — EverLive webcam bridge.
 *
 * Opens a V4L2 webcam once and publishes every frame as YUY2 into an
 * EverLive frame store (/dev/shm/everlive_cam_v4l2_shm by default).  Any
 * number of readers can then use the camera at the same time: the Console
 * Hub's picture-in-picture preview, the recorder (which overlays it onto a
 * capture card's picture) and OBS/Discord through libeverlive-cam-v4l2.so
 * (virtual node /dev/video65).
 *
 * Capture formats, in order of preference: YUYV (passed through), NV12
 * (repacked), MJPEG (decoded with libavcodec when built with it).  A format
 * is only used when the camera offers it at the requested size and rate.
 */

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <glob.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include "everlive_framestore.h"

#ifdef HAVE_LIBAV
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
#endif

#define CAMD_DEFAULT_SHM	"/everlive_cam_v4l2_shm"
#define CAMD_BUFFERS		2
#define CAMD_MAX_W		3840
#define CAMD_MAX_H		2160

static volatile sig_atomic_t g_quit;

struct cam_buffer {
	void *start;
	size_t length;
};

struct cam {
	int fd;
	uint32_t fourcc;
	uint32_t width, height;
	uint32_t bytesperline;
	double fps;
	struct cam_buffer bufs[CAMD_BUFFERS];
	unsigned int nbufs;
#ifdef HAVE_LIBAV
	AVCodecContext *dec;
	AVPacket *pkt;
	AVFrame *frame;
	struct SwsContext *sws;
#endif
};

static void on_signal(int sig)
{
	(void)sig;
	g_quit = 1;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r < 0 && errno == EINTR);
	return r;
}

static const char *fourcc_str(uint32_t f, char out[5])
{
	out[0] = (char)(f & 0xff);
	out[1] = (char)((f >> 8) & 0xff);
	out[2] = (char)((f >> 16) & 0xff);
	out[3] = (char)((f >> 24) & 0xff);
	out[4] = '\0';
	return out;
}

/* ---- device discovery ------------------------------------------------- */

static bool is_capture_device(const char *path, char *name, size_t name_len)
{
	struct v4l2_capability cap;
	int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	bool ok = false;

	if (fd < 0)
		return false;
	memset(&cap, 0, sizeof(cap));
	if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
		uint32_t caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ?
				cap.device_caps : cap.capabilities;

		/* Only enumerate real hardware capture devices (webcams, USB capture cards, etc.);
		 * exclude virtual v4l2 loopback dummy devices. */
		ok = (caps & V4L2_CAP_VIDEO_CAPTURE) && (caps & V4L2_CAP_STREAMING) &&
		     strcmp((const char *)cap.driver, "v4l2 loopback") != 0;
		if (name)
			snprintf(name, name_len, "%s", (const char *)cap.card);
	}
	close(fd);
	return ok;
}

static int list_devices(void)
{
	glob_t g;
	char name[64];
	int found = 0;

	if (glob("/dev/video*", 0, NULL, &g) != 0)
		return 0;
	for (size_t i = 0; i < g.gl_pathc; i++) {
		if (is_capture_device(g.gl_pathv[i], name, sizeof(name))) {
			printf("%s\t%s\n", g.gl_pathv[i], name);
			found++;
		}
	}
	globfree(&g);
	return found;
}

static bool first_device(char *out, size_t len)
{
	glob_t g;
	bool ok = false;

	if (glob("/dev/video*", 0, NULL, &g) != 0)
		return false;
	for (size_t i = 0; i < g.gl_pathc && !ok; i++) {
		if (is_capture_device(g.gl_pathv[i], NULL, 0)) {
			snprintf(out, len, "%s", g.gl_pathv[i]);
			ok = true;
		}
	}
	globfree(&g);
	return ok;
}

/* ---- format negotiation ----------------------------------------------- */

/* Fastest discrete rate the camera offers for fourcc at w x h (0 = none). */
static double best_rate(int fd, uint32_t fourcc, uint32_t w, uint32_t h)
{
	struct v4l2_frmivalenum iv;
	double best = 0.0;

	memset(&iv, 0, sizeof(iv));
	iv.pixel_format = fourcc;
	iv.width = w;
	iv.height = h;
	for (iv.index = 0; xioctl(fd, VIDIOC_ENUM_FRAMEINTERVALS, &iv) == 0; iv.index++) {
		double fps = 0.0;

		if (iv.type == V4L2_FRMIVAL_TYPE_DISCRETE && iv.discrete.numerator)
			fps = (double)iv.discrete.denominator / iv.discrete.numerator;
		else if (iv.stepwise.min.numerator)
			fps = (double)iv.stepwise.min.denominator / iv.stepwise.min.numerator;
		if (fps > best)
			best = fps;
		if (iv.type != V4L2_FRMIVAL_TYPE_DISCRETE)
			break;
	}
	return best;
}

static bool format_offered(int fd, uint32_t fourcc)
{
	struct v4l2_fmtdesc d;

	memset(&d, 0, sizeof(d));
	d.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	for (d.index = 0; xioctl(fd, VIDIOC_ENUM_FMT, &d) == 0; d.index++)
		if (d.pixelformat == fourcc)
			return true;
	return false;
}

static int set_format(struct cam *c, uint32_t fourcc, uint32_t w, uint32_t h,
		      unsigned int fps)
{
	struct v4l2_format f;
	struct v4l2_streamparm p;

	memset(&f, 0, sizeof(f));
	f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	f.fmt.pix.width = w;
	f.fmt.pix.height = h;
	f.fmt.pix.pixelformat = fourcc;
	f.fmt.pix.field = V4L2_FIELD_NONE;
	if (xioctl(c->fd, VIDIOC_S_FMT, &f) < 0 || f.fmt.pix.pixelformat != fourcc)
		return -1;
	if (f.fmt.pix.width > CAMD_MAX_W || f.fmt.pix.height > CAMD_MAX_H)
		return -1;
	c->fourcc = fourcc;
	c->width = f.fmt.pix.width & ~1U;
	c->height = f.fmt.pix.height;
	c->bytesperline = f.fmt.pix.bytesperline;

	memset(&p, 0, sizeof(p));
	p.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	p.parm.capture.timeperframe.numerator = 1;
	p.parm.capture.timeperframe.denominator = fps;
	if (xioctl(c->fd, VIDIOC_S_PARM, &p) == 0 &&
	    p.parm.capture.timeperframe.numerator)
		c->fps = (double)p.parm.capture.timeperframe.denominator /
			 p.parm.capture.timeperframe.numerator;
	else
		c->fps = fps;
	return 0;
}

/*
 * Pick the first of YUYV, NV12, MJPEG that runs at the requested size and
 * rate; if none reaches the rate, the format with the best rate wins.
 */
static int negotiate(struct cam *c, uint32_t w, uint32_t h, unsigned int fps)
{
	uint32_t order[3];
	unsigned int n = 0;
	uint32_t pick = 0;
	double pick_rate = 0.0;

	order[n++] = V4L2_PIX_FMT_YUYV;
	order[n++] = V4L2_PIX_FMT_NV12;
#ifdef HAVE_LIBAV
	order[n++] = V4L2_PIX_FMT_MJPEG;
#endif
	for (unsigned int i = 0; i < n; i++) {
		double rate;

		if (!format_offered(c->fd, order[i]))
			continue;
		rate = best_rate(c->fd, order[i], w, h);
		if (rate + 0.5 >= fps) {
			pick = order[i];
			break;
		}
		if (rate > pick_rate) {
			pick = order[i];
			pick_rate = rate;
		}
	}
	if (!pick) {
		/* No frame-interval info: let the driver decide. */
		for (unsigned int i = 0; i < n && !pick; i++)
			if (format_offered(c->fd, order[i]))
				pick = order[i];
	}
	if (!pick)
		return -1;
	return set_format(c, pick, w, h, fps);
}

/* ---- streaming -------------------------------------------------------- */

static int start_stream(struct cam *c)
{
	struct v4l2_requestbuffers rb;
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	memset(&rb, 0, sizeof(rb));
	rb.count = CAMD_BUFFERS;
	rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	rb.memory = V4L2_MEMORY_MMAP;
	if (xioctl(c->fd, VIDIOC_REQBUFS, &rb) < 0 || rb.count < 2)
		return -1;
	c->nbufs = rb.count > CAMD_BUFFERS ? CAMD_BUFFERS : rb.count;
	for (unsigned int i = 0; i < c->nbufs; i++) {
		struct v4l2_buffer b;

		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		b.index = i;
		if (xioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0)
			return -1;
		c->bufs[i].length = b.length;
		c->bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
					MAP_SHARED, c->fd, b.m.offset);
		if (c->bufs[i].start == MAP_FAILED)
			return -1;
		if (xioctl(c->fd, VIDIOC_QBUF, &b) < 0)
			return -1;
	}
	return xioctl(c->fd, VIDIOC_STREAMON, &type);
}

static void stop_stream(struct cam *c)
{
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

	(void)xioctl(c->fd, VIDIOC_STREAMOFF, &type);
	for (unsigned int i = 0; i < c->nbufs; i++)
		if (c->bufs[i].start && c->bufs[i].start != MAP_FAILED)
			munmap(c->bufs[i].start, c->bufs[i].length);
	c->nbufs = 0;
}

/* ---- conversion to YUY2 ----------------------------------------------- */

static void yuyv_copy(const struct cam *c, const uint8_t *src, uint8_t *dst)
{
	size_t row = (size_t)c->width * 2;
	size_t stride = c->bytesperline ? c->bytesperline : row;

	for (uint32_t y = 0; y < c->height; y++)
		memcpy(dst + y * row, src + y * stride, row);
}

static void nv12_to_yuyv(const struct cam *c, const uint8_t *src, uint8_t *dst)
{
	size_t stride = c->bytesperline ? c->bytesperline : c->width;
	const uint8_t *uv_plane = src + stride * c->height;

	for (uint32_t y = 0; y < c->height; y++) {
		const uint8_t *yr = src + y * stride;
		const uint8_t *uv = uv_plane + (y / 2) * stride;
		uint8_t *o = dst + (size_t)y * c->width * 2;

		for (uint32_t x = 0; x < c->width; x += 2) {
			o[0] = yr[x];
			o[1] = uv[x];
			o[2] = yr[x + 1];
			o[3] = uv[x + 1];
			o += 4;
		}
	}
}

#ifdef HAVE_LIBAV
static int mjpeg_init(struct cam *c)
{
	const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);

	if (!codec)
		return -1;
	c->dec = avcodec_alloc_context3(codec);
	c->pkt = av_packet_alloc();
	c->frame = av_frame_alloc();
	if (!c->dec || !c->pkt || !c->frame)
		return -1;
	c->dec->thread_count = 2;
	return avcodec_open2(c->dec, codec, NULL);
}

static bool mjpeg_to_yuyv(struct cam *c, const uint8_t *src, size_t len, uint8_t *dst)
{
	uint8_t *planes[4] = { dst, NULL, NULL, NULL };
	int strides[4] = { (int)c->width * 2, 0, 0, 0 };

	c->pkt->data = (uint8_t *)src;
	c->pkt->size = (int)len;
	if (avcodec_send_packet(c->dec, c->pkt) < 0)
		return false;
	if (avcodec_receive_frame(c->dec, c->frame) < 0)
		return false;
	c->sws = sws_getCachedContext(c->sws, c->frame->width, c->frame->height,
				      (enum AVPixelFormat)c->frame->format,
				      (int)c->width, (int)c->height,
				      AV_PIX_FMT_YUYV422, SWS_FAST_BILINEAR,
				      NULL, NULL, NULL);
	if (!c->sws)
		return false;
	sws_scale(c->sws, (const uint8_t *const *)c->frame->data,
		  c->frame->linesize, 0, c->frame->height, planes, strides);
	return true;
}

static void mjpeg_free(struct cam *c)
{
	sws_freeContext(c->sws);
	av_frame_free(&c->frame);
	av_packet_free(&c->pkt);
	avcodec_free_context(&c->dec);
}
#endif

/* ---- main ------------------------------------------------------------- */

static void usage(const char *argv0)
{
	printf("Usage: %s [OPTIONS]\n"
	       "  -d, --device PATH   webcam (default: first V4L2 capture device)\n"
	       "  -s, --size WxH      capture size (default 1920x1080)\n"
	       "  -r, --fps N         frame rate (default 30)\n"
	       "      --shm NAME      frame store name (default %s)\n"
	       "  -l, --list          list webcams (path<TAB>name) and exit\n"
	       "  -h, --help          this help\n"
	       "Readers: libeverlive-cam-v4l2.so presents the store as /dev/video65.\n",
	       argv0, CAMD_DEFAULT_SHM);
}

int main(int argc, char **argv)
{
	static const struct option opts[] = {
		{ "device", required_argument, NULL, 'd' },
		{ "size", required_argument, NULL, 's' },
		{ "fps", required_argument, NULL, 'r' },
		{ "shm", required_argument, NULL, 'S' },
		{ "list", no_argument, NULL, 'l' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	char device[256] = "";
	const char *shm = CAMD_DEFAULT_SHM;
	unsigned int req_w = 1920, req_h = 1080, req_fps = 30;
	struct cam c;
	struct everlive_framestore_config cfg;
	struct everlive_framestore *fs = NULL;
	uint8_t *yuyv = NULL;
	uint64_t frames = 0, last_report = 0;
	struct sigaction sa;
	char fcc[5];
	int opt, rc = 1;

	while ((opt = getopt_long(argc, argv, "d:s:r:lh", opts, NULL)) != -1) {
		switch (opt) {
		case 'd': snprintf(device, sizeof(device), "%s", optarg); break;
		case 's':
			if (sscanf(optarg, "%ux%u", &req_w, &req_h) != 2 ||
			    !req_w || !req_h || req_w > CAMD_MAX_W || req_h > CAMD_MAX_H) {
				fprintf(stderr, "[CAM] bad --size '%s'\n", optarg);
				return 2;
			}
			break;
		case 'r':
			req_fps = (unsigned int)strtoul(optarg, NULL, 10);
			if (req_fps < 1 || req_fps > 120)
				req_fps = 30;
			break;
		case 'S': shm = optarg; break;
		case 'l': return list_devices() > 0 ? 0 : 1;
		case 'h': usage(argv[0]); return 0;
		default: usage(argv[0]); return 2;
		}
	}

	if (!device[0] && !first_device(device, sizeof(device))) {
		fprintf(stderr, "[CAM] no webcam found\n");
		return 1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	/* A reader closing the frame pipe must not terminate the bridge. */
	signal(SIGPIPE, SIG_IGN);

	memset(&c, 0, sizeof(c));
	c.fd = open(device, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (c.fd < 0) {
		fprintf(stderr, "[CAM] %s: %s\n", device, strerror(errno));
		return 1;
	}
	if (negotiate(&c, req_w, req_h, req_fps) < 0) {
		fprintf(stderr, "[CAM] %s: no usable format (YUYV/NV12%s)\n", device,
#ifdef HAVE_LIBAV
			"/MJPEG"
#else
			""
#endif
		);
		goto out;
	}
#ifdef HAVE_LIBAV
	if (c.fourcc == V4L2_PIX_FMT_MJPEG && mjpeg_init(&c) < 0) {
		fprintf(stderr, "[CAM] MJPEG decoder unavailable\n");
		goto out;
	}
#endif

	memset(&cfg, 0, sizeof(cfg));
	cfg.name = shm;
	cfg.pci_bdf = "everlive:cam";
	cfg.port_count = 1;
	cfg.slot_count = 3;
	cfg.frame_stride = c.width * c.height * 2U;
	fs = everlive_framestore_create(&cfg);
	yuyv = malloc(cfg.frame_stride);
	if (!fs || !yuyv) {
		fprintf(stderr, "[CAM] cannot create frame store %s\n", shm);
		goto out;
	}
	everlive_framestore_set_geometry(fs, 0, c.width, c.height);

	if (start_stream(&c) < 0) {
		fprintf(stderr, "[CAM] %s: cannot start streaming: %s\n", device,
			strerror(errno));
		goto out;
	}
	printf("[CAM] %s: %ux%u @ %.2f fps %s -> /dev/shm%s\n", device, c.width,
	       c.height, c.fps, fourcc_str(c.fourcc, fcc), shm);
	fflush(stdout);

	rc = 0;
	while (!g_quit) {
		struct pollfd pfd = { .fd = c.fd, .events = POLLIN };
		struct v4l2_buffer b;
		bool ok = false;
		int pr = poll(&pfd, 1, 1000);

		if (pr < 0 && errno == EINTR)
			continue;
		if (pr == 0) {
			everlive_framestore_mark_no_signal(fs, 0);
			continue;
		}
		if (pr < 0 || (pfd.revents & (POLLERR | POLLHUP))) {
			fprintf(stderr, "[CAM] %s: device lost\n", device);
			rc = 3;
			break;
		}
		memset(&b, 0, sizeof(b));
		b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		b.memory = V4L2_MEMORY_MMAP;
		if (xioctl(c.fd, VIDIOC_DQBUF, &b) < 0) {
			if (errno == EAGAIN)
				continue;
			fprintf(stderr, "[CAM] DQBUF: %s\n", strerror(errno));
			rc = 3;
			break;
		}

		/* Drain queue: if newer frames arrived, skip older to keep latency minimal. */
		struct v4l2_buffer next_b;
		memset(&next_b, 0, sizeof(next_b));
		next_b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		next_b.memory = V4L2_MEMORY_MMAP;
		while (xioctl(c.fd, VIDIOC_DQBUF, &next_b) == 0) {
			(void)xioctl(c.fd, VIDIOC_QBUF, &b);
			b = next_b;
			memset(&next_b, 0, sizeof(next_b));
			next_b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
			next_b.memory = V4L2_MEMORY_MMAP;
		}

		if (b.index < c.nbufs && !(b.flags & V4L2_BUF_FLAG_ERROR)) {
			const uint8_t *src = c.bufs[b.index].start;

			if (c.fourcc == V4L2_PIX_FMT_YUYV) {
				yuyv_copy(&c, src, yuyv);
				ok = true;
			} else if (c.fourcc == V4L2_PIX_FMT_NV12) {
				nv12_to_yuyv(&c, src, yuyv);
				ok = true;
			}
#ifdef HAVE_LIBAV
			else if (c.fourcc == V4L2_PIX_FMT_MJPEG) {
				ok = mjpeg_to_yuyv(&c, src, b.bytesused, yuyv);
			}
#endif
		}
		(void)xioctl(c.fd, VIDIOC_QBUF, &b);
		if (ok) {
			everlive_framestore_publish(fs, 0, yuyv, cfg.frame_stride,
						    c.width, c.height, false, true);
			frames++;
		}
		if (frames - last_report >= (uint64_t)(c.fps * 60.0 + 0.5)) {
			last_report = frames;
			printf("[CAM] %llu frames\n", (unsigned long long)frames);
			fflush(stdout);
		}
	}
	stop_stream(&c);
	printf("[CAM] stopped after %llu frames\n", (unsigned long long)frames);
out:
#ifdef HAVE_LIBAV
	if (c.dec)
		mjpeg_free(&c);
#endif
	free(yuyv);
	if (fs)
		everlive_framestore_destroy(fs);
	if (c.fd >= 0)
		close(c.fd);
	return rc;
}
