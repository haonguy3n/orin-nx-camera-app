#!/bin/sh
# Build (and optionally run) the camera-viewer Qt app.
#
#   ./build.sh          build into host-ui/build/
#   ./build.sh run      build, then launch the viewer
#
# Needs system packages: cmake, a C++17 compiler, Qt6 Widgets/Multimedia/
# MultimediaWidgets/Network (+ FFmpeg, pulled in by Qt multimedia).
#   Arch/CachyOS:  sudo pacman -S --needed cmake qt6-base qt6-multimedia
#   Debian/Ubuntu: sudo apt install cmake qt6-base-dev qt6-multimedia-dev
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
if [ "${1:-}" = "run" ]; then
    exec ./build/camera-viewer
fi
