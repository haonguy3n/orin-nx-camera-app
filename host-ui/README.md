# host-ui — camera-viewer

Host-side viewer and device control for the dual-camera device (see
`../DESIGN.md`, sections 4–5). Qt 6 Widgets + Qt Multimedia (QMediaPlayer +
QVideoWidget); no QML, no direct GStreamer dependency. Qt 6 decodes via its FFmpeg media backend, which
handles the device's H.265/H.264 RTSP streams; there are no public low-latency
tuning knobs on QMediaPlayer — acceptable for M1.

## Device addressing

The device enumerates as a USB network interface (CDC-NCM):

- device: `192.168.55.1` (RTSP server on port 8554, mounts `/cam0`, `/cam1`)
- host:   `192.168.55.100` (assigned by the device's dnsmasq)

The URL bar accepts either a bare host/IP (`192.168.55.1` → default port 8554
and mounts) or an `rtsp://host:port` base URL. **Discover** (next to Connect)
broadcasts a UDP discovery request on port **8556** (`../proto/PROTOCOL.md`,
"Discovery") and fills the host box with the replying device's address; if
several devices answer, a popup menu on the button lets you pick one.

## Build

Requires: CMake ≥ 3.16, a C++17 compiler, Qt 6 with the Widgets, Multimedia,
MultimediaWidgets and Network modules (Arch: `qt6-base`, `qt6-multimedia`; make
sure the FFmpeg backend package `qt6-multimedia-ffmpeg` is installed).

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

## Control panel (M2)

A panel on the right of the video panes talks to the device's control channel:
newline-delimited JSON over TCP, port **8555** (protocol reference:
`../proto/PROTOCOL.md`). **Connect** opens it alongside the RTSP streams (same
host as the video; the control port is always 8555). While connected, the panel
polls `get-status` every 2 s (per-camera streaming state, frame counter, and —
while frames flow — the `last_frame` sequence number) and offers per-camera
exposure (µs, 0 = auto), gain (0 = auto), hardware trigger mode and a **Fire**
button (`fire-trigger`, software single trigger — set trigger mode 4 first;
`v4l2` source only). The **Sync trigger** checkbox (`set-sync`) switches every
camera to external trigger mode for hardware-synchronized capture, and reverts
itself if the device refuses. Request errors are shown inline in the Device
status group — no dialogs. Exposure/gain are seeded once from the first
`get-status` after connect.

## Milestone 2 roadmap (remaining)

- Per-pane stream stats and reconnect-on-stall handling.
