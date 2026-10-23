/*
 * overlay.c —— 把毫秒时间戳烧进画面
 *
 * 目的：给玻璃到玻璃延迟一个可观测的标尺。时间戳取自 CLOCK_MONOTONIC
 * （摄像头帧优先用 v4l2 驱动时间戳），画在 Y 平面上，因此会被编码、
 * 传输、解码完整地带到接收端。
 *
 * 同机回环测量：接收端「此刻 - 画面里的时间」就是玻璃到玻璃延迟。
 * 跨设备测量：需要先做两端时钟偏移校准，不能直接相减。
 */

#include "overlay.h"

#include <stdio.h>
#include <string.h>

/* 5x7 点阵，低 5 位有效，最高位在左 */
static const uint8_t F0[7] = { 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E };
static const uint8_t F1[7] = { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E };
static const uint8_t F2[7] = { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F };
static const uint8_t F3[7] = { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E };
static const uint8_t F4[7] = { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 };
static const uint8_t F5[7] = { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E };
static const uint8_t F6[7] = { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E };
static const uint8_t F7[7] = { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 };
static const uint8_t F8[7] = { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E };
static const uint8_t F9[7] = { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C };
static const uint8_t FCOLON[7] = { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 };
static const uint8_t FDOT[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x06 };
static const uint8_t FEQ[7] = { 0x00, 0x1F, 0x00, 0x1F, 0x00, 0x00, 0x00 };
static const uint8_t FF[7] = { 0x06, 0x09, 0x08, 0x1C, 0x08, 0x08, 0x08 };
static const uint8_t FBLANK[7] = { 0, 0, 0, 0, 0, 0, 0 };

static const uint8_t *glyph_rows(char ch)
{
    switch (ch) {
    case '0': return F0;
    case '1': return F1;
    case '2': return F2;
    case '3': return F3;
    case '4': return F4;
    case '5': return F5;
    case '6': return F6;
    case '7': return F7;
    case '8': return F8;
    case '9': return F9;
    case ':': return FCOLON;
    case '.': return FDOT;
    case '=': return FEQ;
    case 'F': return FF;
    default: return FBLANK;
    }
}

/* 填一块矩形：Y 平面填 luma，NV12 的 UV 设成中性 128，避免文字被染色 */
static void fill_rect(AVFrame *f, int x, int y, int w, int h, uint8_t luma)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + w > f->width ? f->width : x + w;
    int y1 = y + h > f->height ? f->height : y + h;
    if (x1 <= x0 || y1 <= y0)
        return;

    for (int yy = y0; yy < y1; yy++)
        memset(f->data[0] + (size_t)yy * f->linesize[0] + x0, luma, x1 - x0);

    if (!f->data[1])
        return;
    int ux0 = x0 & ~1;
    int uw = (x1 - ux0 + 1) & ~1;
    if (uw > f->width - ux0)
        uw = f->width - ux0;
    for (int yy = y0 / 2; yy < (y1 + 1) / 2; yy++)
        memset(f->data[1] + (size_t)yy * f->linesize[1] + ux0, 128, uw);
}

static void draw_text(AVFrame *f, int x, int y, int scale, const char *text)
{
    uint8_t *Y = f->data[0];
    for (const char *p = text; *p; p++) {
        const uint8_t *g = glyph_rows(*p);
        for (int r = 0; r < 7; r++) {
            for (int c = 0; c < 5; c++) {
                if (!(g[r] & (1u << (4 - c))))
                    continue;
                for (int sy = 0; sy < scale; sy++)
                    for (int sx = 0; sx < scale; sx++) {
                        int px = x + c * scale + sx;
                        int py = y + r * scale + sy;
                        if (px >= 0 && px < f->width && py >= 0 && py < f->height)
                            Y[(size_t)py * f->linesize[0] + px] = 255;
                    }
            }
        }
        x += 6 * scale;
    }
}

void overlay_frame(AVFrame *f, int64_t ts_us, uint64_t frame_no)
{
    int scale = f->height >= 720 ? 4 : (f->height >= 480 ? 3 : 2);
    int64_t ms = ts_us / 1000;
    int hh = (int)((ms / 3600000) % 24);
    int mm = (int)((ms / 60000) % 60);
    int ss = (int)((ms / 1000) % 60);
    int milli = (int)(ms % 1000);

    char line1[32], line2[32];
    snprintf(line1, sizeof(line1), "%02d:%02d:%02d.%03d", hh, mm, ss, milli);
    snprintf(line2, sizeof(line2), "F=%06llu", (unsigned long long)frame_no);

    int pad = 8;
    int lh = 7 * scale;
    int box_w = 12 * 6 * scale + 2 * pad;
    int box_h = lh * 2 + 3 * pad;
    fill_rect(f, 16, 16, box_w, box_h, 0);
    draw_text(f, 16 + pad, 16 + pad, scale, line1);
    draw_text(f, 16 + pad, 16 + pad + lh + pad, scale, line2);

    /* 底部 1 秒扫描条：肉眼判断延迟与卡顿 */
    int bar_x = 16, bar_y = f->height - 24;
    int bar_w = f->width - 32, bar_h = 8;
    if (bar_w > 0) {
        fill_rect(f, bar_x, bar_y, bar_w, bar_h, 0);
        int phase = (int)((ts_us % 1000000) * bar_w / 1000000);
        if (phase > 0)
            fill_rect(f, bar_x, bar_y, phase, bar_h, 255);
    }
}
