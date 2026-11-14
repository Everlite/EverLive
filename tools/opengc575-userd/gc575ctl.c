// gc575ctl - talk to the AVerMedia GC575 private UVC extension unit on Linux.
//
// The GC575 exposes a private XU (entity 4, GUID
// 961073c7-49f7-44f2-ab42-e940405940c2) with selector 2 = pending length and
// selector 1 = AT request/response bytes.  This program uses uvcvideo's
// UVCIOC_CTRL_QUERY ioctl, so no libusb and no vendor driver are required.
//
// Build:  cc -O2 -Wall -Wextra -o gc575ctl gc575ctl.c
// Usage:  ./gc575ctl -d /dev/video0 info
//         ./gc575ctl -d /dev/video0 get-index
//         ./gc575ctl -d /dev/video0 set-index 1
//         ./gc575ctl -d /dev/video0 raw 1 23
//
// The protocol framing is reconstructed from the GC575 research notes; this
// tool has not been validated against physical hardware.  It only performs
// the documented control-channel operations.

#include <errno.h>
#include <fcntl.h>
#include <linux/uvcvideo.h>
#include <linux/usb/video.h>
#include <linux/usbdevice_fs.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define GC575_UNIT 4
#define GC575_SEL_LEN 2
#define GC575_SEL_DATA 1
#define GC575_XU_MAX 512

static int usbfs_fd = -1;
static uint8_t usbfs_interface = 0;

static int usbfs_control(uint8_t request_type, uint8_t request, uint16_t value,
                         uint16_t index, void *data, uint16_t length)
{
    struct usbdevfs_ctrltransfer ctrl = {
        .bRequestType = request_type,
        .bRequest = request,
        .wValue = value,
        .wIndex = index,
        .wLength = length,
        .timeout = 2000,
        .data = data,
    };
    int ret = ioctl(usbfs_fd, USBDEVFS_CONTROL, &ctrl);
    if (ret < 0)
        return -errno;
    return ret;
}

static int xu_query(int fd, uint8_t unit, uint8_t selector, uint8_t query,
                    void *data, uint16_t size)
{
    struct uvc_xu_control_query q = {
        .unit = unit,
        .selector = selector,
        .query = query,
        .size = size,
        .data = data,
    };
    if (ioctl(fd, UVCIOC_CTRL_QUERY, &q) < 0)
        return -errno;
    return 0;
}

static int xu_get_len(int fd, uint8_t unit, uint8_t selector, uint16_t *len)
{
    uint8_t buf[2] = {0, 0};
    int ret = xu_query(fd, unit, selector, UVC_GET_LEN, buf, sizeof(buf));
    if (ret < 0)
        return ret;
    *len = (uint16_t)(buf[0] | (buf[1] << 8));
    return 0;
}

static size_t at_build(uint8_t service, uint32_t cmd, const uint8_t *payload,
                       size_t payload_len, uint8_t *out, size_t out_size)
{
    size_t body_len = 4 + payload_len;
    size_t n = 0;
    uint8_t sum = 0;

    if (out_size < body_len + 5)
        return 0;

    out[n++] = (uint8_t)((service - 0x60) & 0xff);

    // Recovered short/long length encoding.  The historical notes flag an
    // anomaly at body length 126, so use the long form from 126 upwards.
    if (body_len < 126) {
        out[n++] = (uint8_t)((body_len + 2) & 0x7f);
        out[n++] = 0x00;
        out[n++] = 0x00;
    } else {
        out[n++] = (uint8_t)(0x80 | ((body_len + 1) >> 8));
        out[n++] = (uint8_t)((body_len + 1) & 0xff);
        out[n++] = 0x00;
    }

    out[n++] = (uint8_t)(cmd & 0xff);
    out[n++] = (uint8_t)((cmd >> 8) & 0xff);
    out[n++] = (uint8_t)((cmd >> 16) & 0xff);
    out[n++] = (uint8_t)((cmd >> 24) & 0xff);

    if (payload_len)
        memcpy(out + n, payload, payload_len);
    n += payload_len;

    for (size_t i = 0; i < n; i++)
        sum = (uint8_t)(sum + out[i]);
    out[n++] = (uint8_t)(-sum);

    return n;
}

static int at_exchange_usbfs(const uint8_t *frame, size_t frame_len,
                             uint8_t *response, size_t response_size,
                             size_t *response_len)
{
    uint8_t lenbuf[2] = {0, 0};
    uint16_t index = (uint16_t)((GC575_UNIT << 8) | usbfs_interface);
    int ret;

    if (frame_len > 0xffff || frame_len > response_size)
        return -E2BIG;

    // SET_CUR selector 2: little-endian outgoing length.
    lenbuf[0] = (uint8_t)(frame_len & 0xff);
    lenbuf[1] = (uint8_t)((frame_len >> 8) & 0xff);
    ret = usbfs_control(0x21, UVC_SET_CUR, GC575_SEL_LEN << 8, index,
                        lenbuf, sizeof(lenbuf));
    if (ret < 0)
        return ret;

    // SET_CUR selector 1: exact AT frame.
    ret = usbfs_control(0x21, UVC_SET_CUR, GC575_SEL_DATA << 8, index,
                        (void *)frame, (uint16_t)frame_len);
    if (ret < 0)
        return ret;

    usleep(20000);

    // GET_CUR selector 2: pending response length.
    memset(lenbuf, 0, sizeof(lenbuf));
    ret = usbfs_control(0xa1, UVC_GET_CUR, GC575_SEL_LEN << 8, index,
                        lenbuf, sizeof(lenbuf));
    if (ret < 0)
        return ret;
    uint16_t pending = (uint16_t)(lenbuf[0] | (lenbuf[1] << 8));
    if (pending == 0)
        pending = 1;
    if (pending > response_size)
        pending = (uint16_t)response_size;

    // GET_CUR selector 1: read exactly the pending length, as captured.
    ret = usbfs_control(0xa1, UVC_GET_CUR, GC575_SEL_DATA << 8, index,
                        response, pending);
    if (ret < 0)
        return ret;

    *response_len = (size_t)ret;
    return 0;
}

static int at_exchange(int fd, uint8_t unit, const uint8_t *frame,
                       size_t frame_len, uint8_t *response, size_t response_size,
                       size_t *response_len)
{
    uint8_t pending[2] = {0, 0};
    uint8_t *tx = NULL;
    uint8_t *rx = NULL;
    uint16_t len_size = 2;
    uint16_t data_size = GC575_XU_MAX;
    int ret;

    if (usbfs_fd >= 0)
        return at_exchange_usbfs(frame, frame_len, response, response_size,
                                 response_len);

    if (frame_len > 0xffff)
        return -E2BIG;

    ret = xu_get_len(fd, unit, GC575_SEL_LEN, &len_size);
    if (ret < 0)
        return ret;
    ret = xu_get_len(fd, unit, GC575_SEL_DATA, &data_size);
    if (ret < 0 || data_size == 0 || data_size > GC575_XU_MAX)
        data_size = GC575_XU_MAX;

    // Selector 2 is the little-endian outgoing length.
    pending[0] = (uint8_t)(frame_len & 0xff);
    pending[1] = (uint8_t)((frame_len >> 8) & 0xff);
    tx = calloc(1, len_size);
    rx = calloc(1, data_size);
    if (!tx || !rx) {
        ret = -ENOMEM;
        goto out;
    }
    memcpy(tx, pending, len_size < 2 ? len_size : 2);
    ret = xu_query(fd, unit, GC575_SEL_LEN, UVC_SET_CUR, tx, len_size);
    if (ret < 0)
        goto out;

    // Selector 1 carries the AT frame.  uvcvideo requires the full control
    // size, so zero-pad the frame to the size reported by GET_LEN.
    memset(tx, 0, len_size);
    free(tx);
    tx = calloc(1, data_size);
    if (!tx) {
        ret = -ENOMEM;
        goto out;
    }
    memcpy(tx, frame, frame_len);
    ret = xu_query(fd, unit, GC575_SEL_DATA, UVC_SET_CUR, tx, data_size);
    if (ret < 0)
        goto out;

    // Read back the pending response length.
    memset(rx, 0, data_size);
    ret = xu_query(fd, unit, GC575_SEL_LEN, UVC_GET_CUR, rx, len_size);
    if (ret < 0)
        goto out;

    uint16_t pending_len = (uint16_t)(rx[0] | (rx[1] << 8));
    if (pending_len > data_size)
        pending_len = data_size;

    // The response may be shorter than the XU control size.  The frame's own
    // header gives the usable length.
    memset(rx, 0, data_size);
    ret = xu_query(fd, unit, GC575_SEL_DATA, UVC_GET_CUR, rx, data_size);
    if (ret < 0)
        goto out;

    size_t actual = pending_len;
    if (data_size >= 2) {
        uint8_t h0 = rx[0], h1 = rx[1];
        if (h0 & 0x80) {
            size_t long_len = (size_t)(((h0 & 0x7f) << 8) | h1) + 4;
            if (long_len <= data_size)
                actual = long_len;
        } else if (h0 >= 2) {
            size_t short_len = (size_t)h0 + 3;
            if (short_len <= data_size)
                actual = short_len;
        }
    }
    if (actual > response_size)
        actual = response_size;
    memcpy(response, rx, actual);
    *response_len = actual;
    ret = 0;

out:
    free(tx);
    free(rx);
    return ret;
}

static void hexdump(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++)
        printf("%02x%s", data[i], i + 1 == len ? "\n" : " ");
}

static int exchange_and_print(int fd, uint8_t unit, uint8_t service,
                              uint32_t cmd, const uint8_t *payload,
                              size_t payload_len, int raw)
{
    uint8_t frame[600];
    uint8_t response[GC575_XU_MAX];
    size_t frame_len, response_len = 0;
    int ret;

    frame_len = at_build(service, cmd, payload, payload_len, frame, sizeof(frame));
    if (frame_len == 0) {
        fprintf(stderr, "command too large\n");
        return 1;
    }

    ret = at_exchange(fd, unit, frame, frame_len, response, sizeof(response),
                      &response_len);
    if (ret < 0) {
        fprintf(stderr, "XU exchange failed: %s (%d)\n", strerror(-ret), ret);
        return 1;
    }

    printf("request : ");
    hexdump(frame, frame_len);
    printf("response: ");
    hexdump(response, response_len);

    if (raw)
        return 0;

    if (response_len < 5 || response[0] != frame[0]) {
        fprintf(stderr, "unexpected response envelope\n");
        return 1;
    }
    uint8_t sum = 0;
    for (size_t i = 0; i + 1 < response_len; i++)
        sum = (uint8_t)(sum + response[i]);
    if ((uint8_t)(sum + response[response_len - 1]) != 0)
        fprintf(stderr, "warning: response checksum mismatch\n");

    // Payload is between the 4-byte envelope header and the checksum.
    const uint8_t *body = response + 4;
    size_t body_len = response_len > 5 ? response_len - 5 : 0;
    printf("payload : ");
    hexdump(body, body_len);

    return 0;
}

static int parse_hex(const char *s, uint8_t *out, size_t max)
{
    size_t n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == ':')
            s++;
        if (!*s)
            break;
        char *end = NULL;
        unsigned long v = strtoul(s, &end, 16);
        if (end == s || v > 0xff)
            return -1;
        out[n++] = (uint8_t)v;
        s = end;
    }
    return (int)n;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-d /dev/videoN] [-D /dev/bus/usb/BBB/DDD] [-u unit] COMMAND\n"
            "\n"
            "  -d PATH   use uvcvideo UVCIOC_CTRL_QUERY (default /dev/video0)\n"
            "  -D PATH   use raw usbfs control transfers (needs permission)\n"
            "  -u N      XU unit/entity id (default 4)\n"
            "  -r        raw output only\n"
            "\n"
            "commands:\n"
            "  info                 AT23 Get_System_Info\n"
            "  get-index            ATb1 Get_USB_decription_Index\n"
            "  set-index 0|1        ATb0 Set_USB_decription_Index\n"
            "  timing               AT37 HDMI RX raw timing (40 bytes)\n"
            "  stable               AT38 HDMI RX video stable\n"
            "  out-timing           AT44 UVC output timing (12 bytes)\n"
            "  vformat              AT45 UVC video format\n"
            "  vfreq                AT86 HDMI RX vertical frequency\n"
            "  raw SERVICE CMD [HEXPAYLOAD]\n"
            "\n"
            "examples:\n"
            "  %s -d /dev/video0 info\n"
            "  %s -d /dev/video0 set-index 1\n"
            "  %s -d /dev/video0 raw 1 23\n",
            argv0, argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
    const char *prog = argv[0];
    const char *device = "/dev/video0";
    const char *usbfs_path = NULL;
    uint8_t unit = GC575_UNIT;
    int opt, raw = 0;

    while ((opt = getopt(argc, argv, "d:D:u:rh")) != -1) {
        switch (opt) {
        case 'd': device = optarg; break;
        case 'D': usbfs_path = optarg; break;
        case 'u': unit = (uint8_t)strtoul(optarg, NULL, 0); break;
        case 'r': raw = 1; break;
        default: usage(prog); return 1;
        }
    }
    argc -= optind;
    argv += optind;
    if (argc < 1) {
        usage(prog);
        return 1;
    }

    int fd = -1;
    if (usbfs_path) {
        usbfs_fd = open(usbfs_path, O_RDWR);
        if (usbfs_fd < 0) {
            fprintf(stderr, "open %s: %s\n", usbfs_path, strerror(errno));
            return 1;
        }
        fd = usbfs_fd;
    } else {
        fd = open(device, O_RDWR);
        if (fd < 0) {
            fprintf(stderr, "open %s: %s\n", device, strerror(errno));
            return 1;
        }
    }

    int ret = 1;
    const char *cmd = argv[0];

    if (!strcmp(cmd, "info")) {
        ret = exchange_and_print(fd, unit, 1, 0x23, NULL, 0, raw);
    } else if (!strcmp(cmd, "get-index")) {
        ret = exchange_and_print(fd, unit, 1, 0xb1, NULL, 0, raw);
    } else if (!strcmp(cmd, "set-index")) {
        if (argc < 2) {
            fprintf(stderr, "set-index needs 0 or 1\n");
            goto out;
        }
        uint32_t index = (uint32_t)strtoul(argv[1], NULL, 0);
        uint8_t payload[8] = {
            0x01, 0x00, 0x00, 0x00,
            (uint8_t)(index & 0xff), (uint8_t)((index >> 8) & 0xff),
            (uint8_t)((index >> 16) & 0xff), (uint8_t)((index >> 24) & 0xff),
        };
        ret = exchange_and_print(fd, unit, 1, 0xb0, payload, sizeof(payload), raw);
    } else if (!strcmp(cmd, "timing")) {
        ret = exchange_and_print(fd, unit, 1, 0x37, NULL, 0, raw);
    } else if (!strcmp(cmd, "stable")) {
        ret = exchange_and_print(fd, unit, 1, 0x38, NULL, 0, raw);
    } else if (!strcmp(cmd, "out-timing")) {
        ret = exchange_and_print(fd, unit, 1, 0x44, NULL, 0, raw);
    } else if (!strcmp(cmd, "vformat")) {
        ret = exchange_and_print(fd, unit, 1, 0x45, NULL, 0, raw);
    } else if (!strcmp(cmd, "vfreq")) {
        ret = exchange_and_print(fd, unit, 1, 0x86, NULL, 0, raw);
    } else if (!strcmp(cmd, "raw")) {
        if (argc < 3) {
            fprintf(stderr, "raw needs SERVICE and CMD\n");
            goto out;
        }
        uint8_t service = (uint8_t)strtoul(argv[1], NULL, 0);
        uint32_t command = (uint32_t)strtoul(argv[2], NULL, 0);
        uint8_t payload[512];
        int payload_len = 0;
        if (argc >= 4) {
            payload_len = parse_hex(argv[3], payload, sizeof(payload));
            if (payload_len < 0) {
                fprintf(stderr, "bad hex payload\n");
                goto out;
            }
        }
        ret = exchange_and_print(fd, unit, service, command, payload,
                                 (size_t)payload_len, raw);
    } else {
        usage(prog);
        goto out;
    }

out:
    if (usbfs_fd >= 0)
        close(usbfs_fd);
    else
        close(fd);
    return ret;
}
