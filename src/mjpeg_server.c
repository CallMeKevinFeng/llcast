/*
 * mjpeg_server.c —— 常驻 MJPEG over HTTP 预览服务
 *
 * 直接读取摄像头的 MJPEG 帧，用 multipart/x-mixed-replace 广播给浏览器，
 * 不做解码/转码。支持多个客户端和断线重连，适合快速确认采集链路。
 */

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#define WIDTH 1280
#define HEIGHT 720
#define NBUF 4
#define MAX_CLIENTS 16

struct buffer {
    void *start;
    size_t length;
};

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r == -1 && errno == EINTR);
    return r;
}

static int open_camera(const char *dev, struct buffer *bufs, int *nbuf)
{
    int fd = open(dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) {
        perror("open camera");
        return -1;
    }

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0 ||
        !(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s 不支持视频采集/流式模式\n", dev);
        close(fd);
        return -1;
    }

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = WIDTH;
    fmt.fmt.pix.height = HEIGHT;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0 ||
        fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG) {
        fprintf(stderr, "摄像头不支持 MJPEG\n");
        close(fd);
        return -1;
    }

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = NBUF;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count < 2) {
        perror("VIDIOC_REQBUFS");
        close(fd);
        return -1;
    }

    for (unsigned i = 0; i < req.count; i++) {
        struct v4l2_buffer b;
        memset(&b, 0, sizeof(b));
        b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        b.memory = V4L2_MEMORY_MMAP;
        b.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0)
            goto fail;
        bufs[i].length = b.length;
        bufs[i].start = mmap(NULL, b.length, PROT_READ | PROT_WRITE,
                             MAP_SHARED, fd, b.m.offset);
        if (bufs[i].start == MAP_FAILED)
            goto fail;
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0)
            goto fail;
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0)
        goto fail;

    *nbuf = (int)req.count;
    return fd;

fail:
    perror("camera setup");
    close(fd);
    return -1;
}

static int send_all(int fd, const void *data, size_t len)
{
    const char *p = data;
    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n <= 0)
            return 0;
        p += n;
        len -= (size_t)n;
    }
    return 1;
}

static int make_listener(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 ||
        listen(fd, 8) < 0) {
        perror("bind/listen");
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN);
    int port = argc > 1 ? atoi(argv[1]) : 8090;
    const char *dev = argc > 2 ? argv[2] : "/dev/video0";

    struct buffer bufs[NBUF];
    int nbuf = 0;
    int cam = open_camera(dev, bufs, &nbuf);
    if (cam < 0)
        return 1;

    int listener = make_listener(port);
    if (listener < 0)
        return 1;

    int clients[MAX_CLIENTS];
    for (int i = 0; i < MAX_CLIENTS; i++)
        clients[i] = -1;

    fprintf(stderr, "[mjpeg] http://0.0.0.0:%d/  %s %dx%d MJPEG\n",
            port, dev, WIDTH, HEIGHT);

    while (1) {
        struct pollfd pfd[2] = {
            { .fd = listener, .events = POLLIN },
            { .fd = cam, .events = POLLIN },
        };
        if (poll(pfd, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (pfd[0].revents & POLLIN) {
            int c = accept(listener, NULL, NULL);
            if (c >= 0) {
                fcntl(c, F_SETFL, O_NONBLOCK);
                for (int i = 0; i < MAX_CLIENTS; i++) {
                    if (clients[i] < 0) {
                        clients[i] = c;
                        const char *hdr =
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: multipart/x-mixed-replace; boundary=llcast\r\n"
                            "Cache-Control: no-cache\r\n"
                            "Connection: close\r\n\r\n";
                        if (!send_all(c, hdr, strlen(hdr))) {
                            close(c);
                            clients[i] = -1;
                        }
                        break;
                    }
                }
            }
        }

        if (pfd[1].revents & POLLIN) {
            struct v4l2_buffer b;
            memset(&b, 0, sizeof(b));
            b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
            b.memory = V4L2_MEMORY_MMAP;
            if (xioctl(cam, VIDIOC_DQBUF, &b) < 0)
                continue;

            size_t len = b.bytesused;
            const char *src = bufs[b.index].start;
            char hdr[160];
            int hl = snprintf(hdr, sizeof(hdr),
                              "--llcast\r\nContent-Type: image/jpeg\r\n"
                              "Content-Length: %zu\r\n\r\n", len);

            for (int i = 0; i < MAX_CLIENTS; i++) {
                if (clients[i] < 0)
                    continue;
                if (!send_all(clients[i], hdr, (size_t)hl) ||
                    !send_all(clients[i], src, len) ||
                    !send_all(clients[i], "\r\n", 2)) {
                    close(clients[i]);
                    clients[i] = -1;
                }
            }

            xioctl(cam, VIDIOC_QBUF, &b);
        }
    }

    return 0;
}
