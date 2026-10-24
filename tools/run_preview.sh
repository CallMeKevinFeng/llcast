#!/usr/bin/env bash
set -euo pipefail

dev="${1:-/dev/video0}"
port="${2:-8090}"
root="$(cd "$(dirname "$0")/.." && pwd)"

exec "$root/build/llcast_mjpeg" "$port" "$dev"
