#!/usr/bin/env bash
set -euo pipefail

dev="${1:-/dev/video0}"
port="${2:-8080}"
root="$(cd "$(dirname "$0")/.." && pwd)"

exec "$root/build/llcast_encode" "$dev" --pipe --frames 0 \
  | "$root/build/llcast_whep" --port "$port"
