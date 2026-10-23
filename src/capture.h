#ifndef LLCAST_CAPTURE_H
#define LLCAST_CAPTURE_H

#include <libavutil/frame.h>

typedef struct capture capture_t;

/* 打开摄像头并尽量协商到 width x height 的 NV12 流，失败返回 NULL */
capture_t *capture_open(const char *device, int width, int height);

/* 取一帧到 frame（NV12）。返回 0 成功；-EAGAIN 超时；其他负数为错误 */
int capture_get_frame(capture_t *c, AVFrame *frame);

/* 读取实际协商出来的分辨率（驱动可能不按请求给） */
void capture_get_size(const capture_t *c, int *width, int *height);

void capture_close(capture_t *c);

#endif
