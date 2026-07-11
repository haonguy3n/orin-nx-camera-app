# host-ui — camera-viewer

Milestone-1 host-side viewer for the dual-camera device (see `../DESIGN.md`,
section 5). Qt 6 Widgets + Qt Multimedia (QMediaPlayer + QVideoWidget); no QML,
no direct GStreamer dependency. Qt 6 decodes via its FFmpeg media backend, which
handles the device's H.265/H.264 RTSP streams; there are no public low-latency
tuning knobs on QMediaPlayer — acceptable for M1.

## Device addressing

The device enumerates as a USB network interface (CDC-NCM):

- device: `192.168.55.1` (RTSP server on port 8554, mounts `/cam0`, `/cam1`)
- host:   `192.168.55.100` (assigned by the device's dnsmasq)

The URL bar accepts either a bare host/IP (`192.168.55.1` → default port 8554
and mounts) or an `rtsp://host:port` base URL.

## Build

Requires: CMake ≥ 3.16, a C++17 compiler, Qt 6 with the Widgets, Multimedia and
MultimediaWidgets modules (Arch: `qt6-base`, `qt6-multimedia`; make sure the
FFmpeg backend package `qt6-multimedia-ffmpeg` is installed).

```sh
cmake -S host-ui -B host-ui/build
cmake --build host-ui/build
./host-ui/build/camera-viewer
```

## Run

1. Plug in the device; wait for the USB network interface (host should get
   `192.168.55.100`, `ping 192.168.55.1` works).
2. Start `camera-viewer`, leave `192.168.55.1` in the URL bar, click
   **Connect**. Each pane shows its own status (connecting / playing /
   error with the QMediaPlayer error message). Click **Disconnect** then
   **Connect** to reconnect (or press Enter in the URL bar to reconnect
   immediately).

## Zero-build fallback

`scripts/preview.sh [device-ip]` opens both streams without building anything:
it prefers `ffplay` (with `-fflags nobuffer -flags low_delay`), falling back to
`gst-launch-1.0 playbin`. Handy for closing M1 before the Qt app is built.

## Testing without the device

Not covered here. Once the device streams (DESIGN.md §6, M1 step 4), simply
point the app (or `preview.sh`) at it. For local development without hardware,
any RTSP server serving H.264/H.265 works, e.g. `mediamtx` fed by an ffmpeg
test source, or GStreamer's `gst-rtsp-server` examples with `videotestsrc !
x264enc`; then enter that server's `rtsp://host:port` in the URL bar (streams
must be mounted at `/cam0` and `/cam1`).

## Milestone 2 roadmap

- Controls pane: exposure, gain, trigger mode via the protobuf-over-TCP control
  channel (shared schema in `../proto/`), alongside the two video panes.
- Per-pane stream stats and reconnect-on-stall handling.
