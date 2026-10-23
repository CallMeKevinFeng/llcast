/*
 * llcast_encode.c —— 低延迟管线第一块积木
 *
 * 采集源二选一：
 *   ./llcast_encode              -> 合成帧（移动条纹）
 *   ./llcast_encode /dev/video0  -> v4l2 摄像头，逐帧采集后编码
 *
 * 用 libav 走通 "采集帧 -> H.264 编码 -> Annex B 文件" 的链路，
 * 并统计每帧 submit+drain 耗时（编码/提交路径耗时，不是玻璃到玻璃延迟）。
 *
 * 优先 NVENC（preset=p1 + tune=ll），打不开时自动回退 libx264。
 *
 * 学习要点：
 *  - Annex B vs AVCC：NVENC 输出 Annex B，libx264 默认 AVCC，这里用 annexb=1 统一。
 *  - 低延迟参数：GOP=30、B 帧=0、NVENC preset=p1 + tune=ll + delay=0。
 *  - v4l2 采集走 mmap + VIDIOC_DQBUF/QBUF 零拷贝轮询（见 capture.c）。
 */

#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>

#include "capture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_W 1280
#define DEFAULT_H 720
#define FPS 30
#define NFRAMES 300

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* 填充一帧：移动的水平条纹 + 一块移动的白条，视觉上能确认画面在动 */
static void fill_frame(AVFrame *f, int idx)
{
    uint8_t *Y = f->data[0];
    uint8_t *UV = f->data[1]; /* NV12 是半平面：data[1] 存交错 UV，data[2] 为 NULL */
    int w = f->width, h = f->height;

    for (int y = 0; y < h; y++)
        memset(Y + y * f->linesize[0], (uint8_t)((y * 2 + idx * 4) & 0xff), w);

    int bx = (idx * 6) % (w - 80);
    for (int y = h / 2 - 40; y < h / 2 + 40; y++)
        for (int x = bx; x < bx + 80; x++)
            Y[y * f->linesize[0] + x] = 255;

    for (int y = 0; y < h / 2; y++)
        memset(UV + y * f->linesize[1], 128, w); /* UV 交错，整行填 128 = 中性灰 */
}

/* 把编码器里可用的包全部写出去，返回写入字节数；EAGAIN/EOF 正常返回 */
static int drain(AVCodecContext *ctx, FILE *out)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt)
        return -1;

    int wrote = 0;
    for (;;) {
        int ret = avcodec_receive_packet(ctx, pkt);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
            break;
        if (ret < 0) {
            av_packet_free(&pkt);
            return ret;
        }
        fwrite(pkt->data, 1, pkt->size, out);
        wrote += pkt->size;
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);
    return wrote;
}

static int configure(AVCodecContext *ctx, const AVCodec *codec, int w, int h)
{
    ctx->width = w;
    ctx->height = h;
    ctx->time_base = (AVRational){ 1, FPS };
    ctx->framerate = (AVRational){ FPS, 1 };
    ctx->pix_fmt = AV_PIX_FMT_NV12;
    ctx->gop_size = 30;      /* 低延迟下 GOP 越小越好，30 = 1 秒 */
    ctx->max_b_frames = 0;   /* B 帧是延迟大户，这里必须为 0 */
    ctx->bit_rate = 2000000;

    if (strstr(codec->name, "nvenc")) {
        av_opt_set(ctx->priv_data, "preset", "p1", 0);   /* p1 = 最快 */
        av_opt_set(ctx->priv_data, "tune", "ll", 0);     /* ll = 低延迟 */
        av_opt_set(ctx->priv_data, "rc", "cbr", 0);
        av_opt_set(ctx->priv_data, "b", "2000000", 0);
        av_opt_set(ctx->priv_data, "bf", "0", 0);
        av_opt_set(ctx->priv_data, "delay", "0", 0);
    } else if (strstr(codec->name, "x264")) {
        av_opt_set(ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(ctx->priv_data, "tune", "zerolatency", 0);
        av_opt_set(ctx->priv_data, "annexb", "1", 0);    /* 统一输出 Annex B */
    }

    return avcodec_open2(ctx, codec, NULL);
}

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    return (x > y) - (x < y);
}

int main(int argc, char **argv)
{
    const char *device = argc > 1 ? argv[1] : NULL;
    capture_t *cap = NULL;
    int w = DEFAULT_W, h = DEFAULT_H;
    int have_capture = 0;

    if (device) {
        cap = capture_open(device, DEFAULT_W, DEFAULT_H);
        if (cap) {
            capture_get_size(cap, &w, &h);
            have_capture = 1;
            printf("[capture] 使用 %s: %dx%d\n", device, w, h);
        } else {
            fprintf(stderr, "[warn] 打开摄像头 %s 失败，回退到合成帧\n", device);
        }
    }

    const char *names[] = { "h264_nvenc", "libx264" };
    const AVCodec *codec = NULL;
    AVCodecContext *ctx = NULL;
    for (int i = 0; i < 2; i++) {
        codec = avcodec_find_encoder_by_name(names[i]);
        if (!codec) {
            fprintf(stderr, "[warn] 编码器 %s 不存在\n", names[i]);
            continue;
        }
        ctx = avcodec_alloc_context3(codec);
        if (!ctx)
            continue;
        int ret = configure(ctx, codec, w, h);
        if (ret == 0)
            break;
        char err[AV_ERROR_MAX_STRING_SIZE] = { 0 };
        av_strerror(ret, err, sizeof(err));
        fprintf(stderr, "[warn] 打开 %s 失败: %s，尝试下一个\n", names[i], err);
        avcodec_free_context(&ctx);
        ctx = NULL;
    }
    if (!ctx) {
        fprintf(stderr, "没有可用的 H.264 编码器，请确认 ffmpeg 编入了 nvenc 或 libx264\n");
        return 1;
    }
    printf("使用编码器: %s\n", codec->name);

    AVFrame *frame = av_frame_alloc();
    if (!frame)
        return 1;
    frame->format = AV_PIX_FMT_NV12;
    frame->width = w;
    frame->height = h;
    if (av_frame_get_buffer(frame, 32) < 0) {
        fprintf(stderr, "分配帧缓冲失败\n");
        return 1;
    }

    FILE *out = fopen("out.h264", "wb");
    if (!out) {
        perror("fopen out.h264");
        return 1;
    }

    /* NVENC 的 extradata 是 Annex B 的 SPS/PPS，直接写；x264 已通过 annexb=1 统一 */
    if (ctx->extradata && ctx->extradata_size)
        fwrite(ctx->extradata, 1, ctx->extradata_size, out);

    double *lats = calloc(NFRAMES, sizeof(double));
    if (!lats)
        return 1;
    int n = 0;
    int64_t total_bytes = ctx->extradata_size;

    for (int i = 0; i < NFRAMES; i++) {
        if (have_capture) {
            int r = -EAGAIN;
            for (int tries = 0; tries < 5 && r == -EAGAIN; tries++)
                r = capture_get_frame(cap, frame);
            if (r < 0) {
                fprintf(stderr, "[warn] 取帧失败，改用合成帧\n");
                have_capture = 0;
                fill_frame(frame, i);
            }
        } else {
            fill_frame(frame, i);
        }
        frame->pts = i;

        int64_t t0 = now_ns();
        int ret = avcodec_send_frame(ctx, frame);
        while (ret == AVERROR(EAGAIN)) {
            total_bytes += drain(ctx, out);
            ret = avcodec_send_frame(ctx, frame);
        }
        if (ret < 0) {
            char err[AV_ERROR_MAX_STRING_SIZE] = { 0 };
            av_strerror(ret, err, sizeof(err));
            fprintf(stderr, "send_frame 失败: %s\n", err);
            return 1;
        }
        total_bytes += drain(ctx, out);
        int64_t t1 = now_ns();

        lats[n++] = (double)(t1 - t0) / 1000.0; /* 微秒 */
        if ((i + 1) % 60 == 0)
            printf("frame %4d  submit+drain %.1f us\n", i + 1, lats[n - 1]);
    }

    /* flush：送入 NULL，把编码器内部剩余的包全部取出 */
    avcodec_send_frame(ctx, NULL);
    total_bytes += drain(ctx, out);

    fclose(out);
    if (cap)
        capture_close(cap);
    av_frame_free(&frame);
    avcodec_free_context(&ctx);

    double sum = 0, mx = 0;
    for (int i = 0; i < n; i++) {
        sum += lats[i];
        if (lats[i] > mx)
            mx = lats[i];
    }
    double *sorted = calloc(n, sizeof(double));
    memcpy(sorted, lats, n * sizeof(double));
    qsort(sorted, n, sizeof(double), cmp_double);

    printf("---\n");
    printf("帧数: %d\n", n);
    printf("submit+drain 耗时  avg=%.1f us  p50=%.1f us  p95=%.1f us  max=%.1f us\n",
           sum / n, sorted[n / 2], sorted[(int)(n * 0.95)], mx);
    printf("输出: out.h264  %.2f MB  (%.2f Mbps)\n",
           total_bytes / 1000000.0, total_bytes * 8.0 / (NFRAMES / (double)FPS) / 1000000.0);
    printf("说明: 这是编码/提交路径耗时，玻璃到玻璃延迟还需叠加采集、网络、抖动缓冲、解码、渲染\n");

    free(lats);
    free(sorted);
    return 0;
}
