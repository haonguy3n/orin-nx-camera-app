#!/bin/sh
# Zero-build fallback viewer for milestone 1 verification.
# Opens both device RTSP streams with whatever player is installed:
# ffplay (preferred, low-latency flags) or gst-launch-1.0 playbin.
#
# Usage: preview.sh [device-ip]   (default 192.168.55.1)

set -u

HOST="${1:-192.168.55.1}"
URL0="rtsp://${HOST}:8554/cam0"
URL1="rtsp://${HOST}:8554/cam1"

PIDS=""
cleanup() {
    for pid in $PIDS; do
        kill "$pid" 2>/dev/null
    done
}
trap cleanup INT TERM EXIT

if command -v ffplay >/dev/null 2>&1; then
    echo "Using ffplay: $URL0 / $URL1"
    ffplay -hide_banner -fflags nobuffer -flags low_delay \
        -window_title cam0 "$URL0" &
    PIDS="$PIDS $!"
    ffplay -hide_banner -fflags nobuffer -flags low_delay \
        -window_title cam1 "$URL1" &
    PIDS="$PIDS $!"
elif command -v gst-launch-1.0 >/dev/null 2>&1; then
    echo "Using gst-launch-1.0 playbin: $URL0 / $URL1"
    gst-launch-1.0 playbin uri="$URL0" &
    PIDS="$PIDS $!"
    gst-launch-1.0 playbin uri="$URL1" &
    PIDS="$PIDS $!"
else
    echo "error: neither ffplay nor gst-launch-1.0 found" >&2
    exit 1
fi

wait
