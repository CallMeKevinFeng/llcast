#ifndef LLCAST_OVERLAY_H
#define LLCAST_OVERLAY_H

#include <libavutil/frame.h>
#include <stdint.h>

/* 把采集时间戳（单调时钟，微秒）和帧号画到 NV12 帧左上角 */
void overlay_frame(AVFrame *frame, int64_t ts_us, uint64_t frame_no);

#endif
