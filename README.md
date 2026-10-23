# llcast —— 低延迟管线第一块积木

最短链路：合成帧 -> H.264 编码 -> Annex B 文件，并统计每帧 submit+drain 耗时。

## 构建

```bash
cmake -S . -B build
cmake --build build -j
./build/llcast_encode
```

依赖：`libavcodec-dev`、`libavutil-dev`（FFmpeg 7.x，Ubuntu 26.04 自带）。
需要 ffmpeg 编入 `h264_nvenc`（配合 nv-codec-headers）或 `libx264`。

## 验证

```bash
ffprobe -v error -f h264 -show_entries stream=codec_name,width,height -of default=noprint_wrappers=1 out.h264
ffplay out.h264
```

看到移动的条纹和白条就说明管线通了。

## 接真实摄像头

```bash
sudo apt install v4l2-utils   # 提供 v4l2-ctl 查看设备
v4l2-ctl --list-devices
./build/llcast_encode /dev/video0
```

采集模块用 v4l2 mmap 零拷贝，优先 NV12（直接喂 NVENC），摄像头只给 YUYV 时自动转 NV12；
打开失败会回退到合成帧。只支持 MJPEG 的摄像头待后续加解码支持。

## 关于这台机器（RTX 1080 Ti / GP102 / Pascal）

- NVENC 支持 H.264 与 H.265 8-bit 编码；不支持 AV1（AV1 编码需 RTX 40 系）。
- Pascal 的 NVENC 没有 B 帧，画质是较老一代（Turing 之后在同码率下明显更好），
  但低延迟场景 B 帧本来就是 0，所以不影响我们的目标。
- 消费级卡 NVENC 并发会话数有限（Pascal 约 3 路），当前单路演示不受影响；
  将来做多路转码再考虑这个限制。

## 低延迟参数速查

NVENC：`-preset p1 -tune ll -rc cbr -b:v 2M -g 30 -bf 0 -delay 0`

## 下一步里程碑

1. v4l2 采集替换合成帧（`src/capture.c`）。
2. RTP 打包 + WHIP 出流（`src/rtp.c`、`src/ingest.c`）。
3. 毫秒计时器叠加到画面，做玻璃到玻璃实测。
4. tc netem 丢包注入 + 弱网矩阵，产出延迟/卡顿数据表。
