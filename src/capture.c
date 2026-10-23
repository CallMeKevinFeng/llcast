/*
 * capture.c —— v4l2 mmap 零拷贝采集
 *
 * 流程：open -> VIDIOC_S_FMT 协商格式 -> VIDIOC_REQBUFS + mmap ->
 *       VIDIOC_QBUF 全部入队 -> VIDIOC_STREAMON -> 循环 DQBUF/QBUF。
 *
 * 摄像头最常给 YUYV，少数给 NV12；这里优先 NV12（直接喂 NVENC），
 * YUYV 则用 libswscale 转成 NV12。只支持 MJPEG 的摄像头以后再补（需要先解码）。
 */

#include "capture.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <libswscale/swscale.h>

#define NBUF 4

struct buf {
    void *start;
    size_t length;
};

struct capture {
    int fd;
    int width, height;
    uint32_t src_pixfmt;    /* 摄像头原生像素格式 */
    size_t bytesperline;    /* 原生行跨度 */
    struct buf bufs[NBUF];
    unsigned nbuf;
    struct SwsContext *sws; /* YUYV -> NV12，NV12 原生时为 NULL */
    int64_t last_ts_us;
};

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

capture_t *capture_open(const char *device, int width, int height)
{
    capture_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;
    c->fd = -1;

    c->fd = open(device, O_RDWR | O_NONBLOCK);
    if (c->fd < 0) {
        perror("open");
        goto fail;
    }

    struct v4l2_capability vcap;
    memset(&vcap, 0, sizeof(vcap));
    if (xioctl(c->fd, VIDIOC_QUERYCAP, &vcap) < 0) {
        perror("VIDIOC_QUERYCAP");
        goto fail;
    }
    if (!(vcap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s 不支持视频采集\n", device);
        goto fail;
    }
    if (!(vcap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s 不支持 mmap 流式采集\n", device);
        goto fail;
    }

    /* 先请求 NV12；多数 UVC 驱动会回退到自己的默认格式而不是报错 */
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("VIDIOC_S_FMT");
        goto fail;
    }

    if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_NV12) {
        c->src_pixfmt = V4L2_PIX_FMT_NV12;
    } else if (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_YUYV) {
        /* 驱动回退到了 YUYV，明确再请求一次，确保格式稳定 */
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (xioctl(c->fd, VIDIOC_S_FMT, &fmt) < 0) {
            perror("VIDIOC_S_FMT(YUYV)");
            goto fail;
        }
        c->src_pixfmt = fmt.fmt.pix.pixelformat;
    } else {
        fprintf(stderr, "%s 不支持 NV12/YUYV，协商到 0x%08x\n",
                device, fmt.fmt.pix.pixelformat);
        goto fail;
    }

    c->width = fmt.fmt.pix.width;
    c->height = fmt.fmt.pix.height;
    c->bytesperline = fmt.fmt.pix.bytesperline;
    fprintf(stderr, "[capture] %s: %dx%d, 格式 0x%08x\n",
            device, c->width, c->height, c->src_pixfmt);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = NBUF;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(c->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("VIDIOC_REQBUFS");
        goto fail;
    }
    if (req.count < 2) {
        fprintf(stderr, "mmap 缓冲不足\n");
        goto fail;
    }
    c->nbuf = req.count;

    for (unsigned i = 0; i < c->nbuf; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(c->fd, VIDIOC_QUERYBUF, &b) < 0) {
            perror("VIDIOC_QUERYBUF");
            goto fail;
        }
        c->bufs[i].length = b.length;
        c->bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, c->fd, b.m.offset);
        if (c->bufs[i].start == MAP_FAILED) {
            perror("mmap");
            c->bufs[i].start = NULL;
            goto fail;
        }
        if (xioctl(c->fd, VIDIOC_QBUF, &b) < 0) {
            perror("VIDIOC_QBUF");
            goto fail;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(c->fd, VIDIOC_STREAMON, &type) < 0) {
        perror("VIDIOC_STREAMON");
        goto fail;
    }

    if (c->src_pixfmt == V4L2_PIX_FMT_YUYV) {
        c->sws = sws_getContext(c->width, c->height, AV_PIX_FMT_YUYV422,
                                c->width, c->height, AV_PIX_FMT_NV12,
                                SWS_FAST_BILINEAR, NULL, NULL, NULL);
        if (!c->sws) {
            fprintf(stderr, "创建 swscale 上下文失败\n");
            goto fail;
        }
    }

    return c;

fail:
    capture_close(c);
    return NULL;
}

int capture_get_frame(capture_t *c, AVFrame *frame)
{
    struct pollfd pfd = { .fd = c->fd, .events = POLLIN };
    int pr = poll(&pfd, 1, 1000); /* 1 秒超时 */
    if (pr == 0)
        return -EAGAIN;
    if (pr < 0)
        return -errno;

    struct v4l2_buffer b;
    memset(&b, 0, sizeof(b));
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    if (xioctl(c->fd, VIDIOC_DQBUF, &b) < 0)
        return -errno;

    if ((b.flags & V4L2_BUF_FLAG_TIMESTAMP_MASK) == V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC)
        c->last_ts_us = (int64_t)b.timestamp.tv_sec * 1000000 + b.timestamp.tv_usec;
    else
        c->last_ts_us = now_us();

    const uint8_t *src = c->bufs[b.index].start;

    if (c->src_pixfmt == V4L2_PIX_FMT_NV12) {
        for (int y = 0; y < c->height; y++)
            memcpy(frame->data[0] + y * frame->linesize[0],
                   src + y * c->bytesperline, c->width);
        const uint8_t *uv = src + c->bytesperline * c->height;
        for (int y = 0; y < c->height / 2; y++)
            memcpy(frame->data[1] + y * frame->linesize[1],
                   uv + y * c->bytesperline, c->width);
    } else { /* YUYV -> NV12 */
        const uint8_t *in_data[4] = { src, NULL, NULL, NULL };
        int in_linesize[4] = { (int)c->bytesperline, 0, 0, 0 };
        sws_scale(c->sws, in_data, in_linesize, 0, c->height,
                  frame->data, frame->linesize);
    }

    xioctl(c->fd, VIDIOC_QBUF, &b);
    return 0;
}

int64_t capture_timestamp_us(const capture_t *c)
{
    return c->last_ts_us;
}

void capture_get_size(const capture_t *c, int *width, int *height)
{
    if (width)
        *width = c->width;
    if (height)
        *height = c->height;
}

void capture_close(capture_t *c)
{
    if (!c)
        return;
    if (c->fd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(c->fd, VIDIOC_STREAMOFF, &type);
    }
    for (unsigned i = 0; i < c->nbuf; i++)
        if (c->bufs[i].start)
            munmap(c->bufs[i].start, c->bufs[i].length);
    if (c->sws)
        sws_freeContext(c->sws);
    if (c->fd >= 0)
        close(c->fd);
    free(c);
}
