# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Camera device software for a Jetson Orin NX with dual VC MIPI IMX296 sensors:
`embedded/` runs on the device (`camera-streamer`, C++20, GStreamer/GLib),
`host-ui/` is the Qt6 viewer (`camera-viewer`), `yocto/meta-vc-camera/` is the
Yocto layer that ships it all. `common/` holds code compiled into both sides.

## Build & test

```sh
# embedded service (host build; needs gstreamer, gst-rtsp-server, json-glib,
# OpenSSL + OpenCV when ENABLE_SECURE_USB=ON, the default)
cd embedded && cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build

# run without camera hardware (videotestsrc + software encode)
./build/camera-streamer config/test-videotestsrc.conf

# host viewer (needs qt6-multimedia, libusb, openssl, gst-libav)
cd host-ui && ./build.sh run

# device image: built by the separate yb/kas project, not this repo
cd ~/Projects/orin-nx && yb build orin-nx.yml
```

There is no test framework. Tests are standalone assert-based self-checks in
`*/test/*.cpp`; each file's header comment holds its exact one-line `g++`
compile-and-run command. Run one by copying that line, e.g. from
`embedded/src/camera/base/test/base_check.cpp` (run from `embedded/`) or
`common/secure/test/handshake_trust_check.cpp` (run from repo root). New
non-trivial logic gets the same kind of check.

## Hardware / operational hazards

- **Never stop or restart `camera-streamer` on the device over SSH in usb
  mode** — teardown unbinds the USB gadget, which kills the NCM interface your
  SSH session rides on.
- Leave changes uncommitted until the user confirms they build and work on
  hardware.
- `camera_overrides.isp` changes only apply after a camera-session restart;
  `set-isp` element properties apply live.
- Propose additions to the Yocto image scope; don't just add them.

## Architecture

All documentation lives in `docs/`: `DESIGN.md` (system design, milestones),
`TRANSPORT-ARCHITECTURE.md` (transport split, class→file map, threading),
`PROTOCOL.md` (JSON control protocol), `EMBEDDED.md` (config keys, service
layout), `HOST-UI.md`, `YOCTO.md`, `ISP-TUNING.md`.

**One transport at a time.** Argus allows a single consumer per camera, so
`[server] transports` in `/etc/camera-streamer.conf` picks the mode:

- `usb` (default): everything — H.265 video, control, firmware update, face
  boxes — is multiplexed over raw USB endpoints (FunctionFS gadget) inside an
  encrypted session (ECDHE-P256 handshake pinned to the device cert,
  ChaCha20-Poly1305 records). No TCP/UDP sockets except the 8557 recovery
  listener. Device side: `embedded/src/camera/secure/SecureUsbServer.cpp`;
  host side: `host-ui/secureusbbridge.cpp`; shared handshake/wire crypto:
  `common/secure/`.
- `network`: classic RTSP (8554) + TCP control (8555) + UDP discovery (8556) +
  OTA upload (8557), for VLC/ffmpeg interop. Video unencrypted.

**Shared above the split**: `media::CameraPipeline` (one per sensor, owns the
GStreamer pipeline and frame fanout — nothing else may open the sensor),
YuNet face detection on GPU (`detect/FaceDetector`), and `detect::IMetaSink`
as the only per-mode difference in box delivery (`SessionMetaSink` →
`Channel::Meta` on usb; `ControlMetaSink` → broadcast control event on
network). Video and detection pump on separate threads — inference is slower
than the frame interval.

**Control protocol**: newline-delimited JSON-RPC; each method is one handler
class in `embedded/src/camera/control/handlers/` registered in
`RegisterHandlers.cpp`. Adding a method = new handler + register call.
Port/method/error constants shared by both sides live in
`common/proto/Protocol.h` — the single source of truth.

**Yocto layer** (`yocto/meta-vc-camera/`): VC MIPI kernel driver + DT patches,
USB gadget service, measured ISP tuning (`camera_overrides.isp` — carries the
black-level fix; without it the image has a pink haze), swupdate A/B image.
Consumed by the `~/Projects/orin-nx` build, see `docs/YOCTO.md`.

## Code conventions (embedded)

folly/fboss style: CamelCase files, lowercase dirs, `namespace camera`,
includes rooted at `src/` (`#include "camera/rtsp/RtspServer.h"`).
`src/camera/base/` is a vendored folly-API mimic layer (GLib-backed, **not**
real folly — never add a folly dependency). House rules that follow:

- Fallible functions return `folly::Expected<T, std::string>` (or
  `ControlError`) — no bool + out-param.
- Raw fds in `folly::File`; cross-thread state in `folly::Synchronized`;
  cleanup via `SCOPE_EXIT`; logging via `XLOGF`.
- TLS misconfiguration is a fatal startup error — never fall back to
  plaintext silently.
- The `queue` elements in GStreamer pipelines are load-bearing (NVMM buffer
  starvation); don't remove them.
