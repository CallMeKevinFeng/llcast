#!/usr/bin/env bash
set -euo pipefail

dev="${1:-/dev/video0}"
port="${2:-8090}"

# ffmpeg 的 mpjpeg listen 是单连接模式；断开后自动重启，方便刷新页面
while true; do
  ffmpeg -hide_banner -loglevel error -f v4l2 -input_format mjpeg \
    -video_size 1280x720 -framerate 30 -i "$dev" -c:v copy \
    -f mpjpeg -listen 1 "http://0.0.0.0:${port}" || true
  sleep 1
done
