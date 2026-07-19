#!/bin/sh
# Build (and optionally run) the camera-viewer Qt app.
#
#   ./build.sh          build into host-ui/build/
#   ./build.sh run      build, then launch the viewer
#
# For secure USB, `run` needs a trust anchor in CAMERA_SECURE_USB_CERT. It is
# found automatically if one of the usual locations holds a ca.crt; override
# by exporting CAMERA_SECURE_USB_CERT yourself. Either works:
#   ca.crt      - any camera signed by that CA is accepted (scripts/
#                 provision-device-cert.sh)
#   server.crt  - that one device only, i.e. certificate pinning
#
# Needs system packages: cmake, a C++17 compiler, Qt6 Widgets/Multimedia/
# MultimediaWidgets/Network (+ FFmpeg), libusb, OpenSSL, and the GStreamer
# app library plus gst-libav at runtime (avdec_h265 decodes the secure USB
# stream in-process).
#   Arch/CachyOS:  sudo pacman -S --needed cmake qt6-base qt6-multimedia \
#                      libusb openssl gstreamer gst-plugins-base gst-libav
#   Debian/Ubuntu: sudo apt install cmake qt6-base-dev qt6-multimedia-dev \
#                      libusb-1.0-0-dev libssl-dev \
#                      libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
#                      gstreamer1.0-libav
set -eu

cd "$(dirname "$0")"

missing=""
command -v cmake >/dev/null 2>&1 || missing="cmake"
command -v c++   >/dev/null 2>&1 || missing="$missing c++-compiler"
if [ -n "$missing" ]; then
    echo "error: missing build tools:$missing" >&2
    echo "  Arch/CachyOS: sudo pacman -S --needed cmake gcc" >&2
    exit 1
fi

if ! cmake -B build -S . -DCMAKE_BUILD_TYPE=Release; then
    echo "" >&2
    echo "Configure failed - usually a missing Qt6 module. Install with:" >&2
    echo "  Arch/CachyOS: sudo pacman -S --needed qt6-base qt6-multimedia" >&2
    echo "  Debian/Ubuntu: sudo apt install qt6-base-dev qt6-multimedia-dev" >&2
    exit 1
fi
cmake --build build -j"$(nproc)"

echo ""
echo "Built: $(pwd)/build/camera-viewer"
[ "${1:-}" = "run" ] || exit 0

# Locate a trust anchor unless one was given. Nothing here generates or picks
# a key -- an unfound anchor is reported, never silently skipped, because
# secure USB refuses to pair without one.
if [ -z "${CAMERA_SECURE_USB_CERT:-}" ]; then
    for candidate in \
        "$HOME"/camera-certs/*/ca.crt \
        "$HOME"/.config/camera-viewer/ca.crt \
        "$HOME"/.config/camera-ca/ca.crt \
        "$HOME"/.config/camera-viewer/device.crt
    do
        if [ -r "$candidate" ]; then
            CAMERA_SECURE_USB_CERT="$candidate"
            export CAMERA_SECURE_USB_CERT
            echo "Trust anchor: $CAMERA_SECURE_USB_CERT"
            break
        fi
    done
fi
if [ -z "${CAMERA_SECURE_USB_CERT:-}" ]; then
    echo "" >&2
    echo "warning: no trust anchor found - secure USB will not pair." >&2
    echo "  Issue one:  ../scripts/provision-device-cert.sh --device-id <id> \\" >&2
    echo "                  --out ~/camera-certs/<id>" >&2
    echo "  Or point at an existing cert:" >&2
    echo "    CAMERA_SECURE_USB_CERT=/path/to/ca.crt ./build.sh run" >&2
    echo "  The viewer still runs; it falls back to the network transport." >&2
fi

# Exactly one viewer may hold the secure USB interface. A stale instance
# keeps the device's session alive, and a second viewer's SET_INTERFACE
# resets the endpoints under it -- both sides then fail confusingly.
for pid in $(pgrep -x camera-viewer 2>/dev/null); do
    echo "warning: camera-viewer already running (pid $pid); closing it" >&2
    kill "$pid" 2>/dev/null || true
done

exec ./build/camera-viewer
