# camera-streamer

RTSP streaming service for the Jetson Orin NX camera device (milestones 1-2,
see `../DESIGN.md`). Serves one hardware-encoded H.265/H.264 stream per
camera, plus a TCP control channel for runtime camera settings:

```
rtsp://192.168.55.1:8554/cam0
rtsp://192.168.55.1:8554/cam1
tcp://192.168.55.1:8555        (control protocol, ../proto/PROTOCOL.md)
udp  192.168.55.1:8556         (discovery responder)
tcp://192.168.55.1:8557        (OTA .swu upload -> swupdate)
```

The control and update channels can be wrapped in TLS/mTLS — see
"TLS (secure USB)" below. Port and method constants shared with the host
UI live in `../common/proto/Protocol.h`.

## Build

Dependencies: CMake >= 3.16, a C++17 compiler, and dev packages for
`gstreamer-1.0`, `gstreamer-rtsp-server-1.0` and `json-glib-1.0`
(Debian/Ubuntu: `libgstreamer1.0-dev libgstrtspserver-1.0-dev
libjson-glib-dev`; Arch: `gstreamer` `gst-rtsp-server` `json-glib`).

```sh
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
sudo cmake --install build     # optional: bin, /etc/camera-streamer.conf, systemd unit
```

The production (target) build is done by the Yocto recipe
`meta-vc-camera/recipes-apps/camera-streamer` (`inherit cmake pkgconfig
systemd`) — no manual cross-compilation needed.

## Configuration

INI file at `/etc/camera-streamer.conf`, or pass a path as the first argument:

```sh
camera-streamer /path/to/other.conf
```

If the file is missing, built-in defaults are used (both cameras enabled,
Argus sensors 0/1, 1440x1080@60, H.265 @ 8 Mbit/s). A commented default file
ships in `config/camera-streamer.conf`. All keys:

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[server]` | `port` | `8554` | RTSP listen port |
| | `listen` | `all` | network to serve on: `all` (USB + ethernet at once), `usb` (usb0 only), `ethernet` (eth0 only), or an explicit IPv4 address / interface name |
| | `transport` | `tcp` | RTP transport offered: `tcp` (interleaved, firewall-proof), `udp`, or `all` (client picks) |
| | `control-port` | `8555` | TCP control protocol port, same bind address as RTSP; `0` disables |
| | `discovery-port` | `8556` | UDP discovery responder (always 0.0.0.0); `0` disables |
| | `update-port` | `8557` | OTA `.swu` upload server (binary, streamed into swupdate IPC); `0` disables |
| | `tls-cert` / `tls-key` | unset | PEM certificate + key; setting both enables TLS on the control and update ports. Setting only one, or an unreadable file, is a **fatal startup error** — no silent plaintext fallback |
| | `tls-ca` | unset | additionally **require** a client certificate signed by this CA (mTLS); only authorized host software can command the device |
| `[cam0]`/`[cam1]` | `enabled` | `true` | serve this camera at `/cam0`/`/cam1` |
| | `source` | `argus` | `argus` (nvarguscamerasrc/ISP), `v4l2` (v4l2src, best-effort in M1), `test` (videotestsrc + software encoder, no hardware needed) |
| | `sensor-id` | `0` / `1` | argus only: nvarguscamerasrc sensor-id |
| | `device` | `/dev/video0` / `/dev/video1` | v4l2 only: capture device |
| | `width` | `1440` | capture width |
| | `height` | `1080` | capture height |
| | `framerate` | `60` | frames per second (integer) |
| | `codec` | `h265` | `h265` or `h264` |
| | `bitrate` | `8000000` | encoder target, bit/s |
| | `exposure` | `0` | µs; `0` = auto (argus) / driver default (v4l2) |
| | `gain` | `0` | `0` = auto/default; argus: analog gain multiplier, v4l2: raw VC driver units (milli-dB, 0–48000 = 0–48 dB) |
| | `trigger` | `-1` | VC hardware trigger mode 0–7, v4l2 source only; `-1` = leave driver default |
| | `isp-<property>` | — | argus only: preset an nvarguscamerasrc ISP property (`isp-wbmode=1`, `isp-saturation=1.2`, …); same whitelist as the protocol's `set-isp` |
| | `zoom` | `1.0` | digital zoom 1–8× (GPU crop + upscale; 1.0 = converter not in the pipeline) |
| | `zoom-x` / `zoom-y` | `0.5` | crop center as a fraction of the frame (pan while zoomed) |

The final GStreamer launch string for every mount is logged at startup —
useful for reproducing issues with plain `gst-launch-1.0`.

## Control protocol (M2)

`../proto/PROTOCOL.md` defines the newline-delimited JSON protocol on port
8555: `get-status` (per-camera streaming state, frame counters, live AE
exposure/gain readback, and `last_frame` capture metadata for cross-camera
sync checks), `set-exposure` / `set-gain` / `set-trigger`, `set-isp`
(runtime WB/saturation/TNR/EE), `set-zoom` (digital zoom + pan), `set-sync`
(all cameras to external trigger) / `fire-trigger` (software single
trigger), generic `list-controls`/`get-control`/`set-control` for
everything else the VC driver exposes, `reload`, `reboot`, and
`get-update-status` (swupdate install progress). A UDP responder on
port 8556 answers `{"method":"discover"}` broadcasts so the host UI can
find devices, and port 8557 accepts `.swu` firmware uploads (streamed
straight into swupdate's IPC socket, no temp file — see PROTOCOL.md
"OTA firmware update"). The TCP side is plain enough to drive by hand:

```sh
printf '{"id":1,"method":"set-exposure","params":{"camera":0,"us":5000}}\n' \
    | nc -q1 192.168.55.1 8555
```

Exposure/gain on the `argus` path pin the matching `nvarguscamerasrc` 3A
range on the live pipeline (and seed future pipelines); on the `v4l2` path
they go straight to the VC driver's V4L2 controls. Hardware trigger modes
are v4l2-only — Argus owns the sensor timing.

## TLS (secure USB)

The channels that *command* the device (control 8555, update 8557) can be
TLS-wrapped; RTSP and discovery are unaffected. Enable by setting
`tls-cert` + `tls-key` in `[server]`; add `tls-ca` to also require a
client certificate signed by that CA (mTLS), so only authorized host
software can control the camera. A self-signed EC P-256 device
certificate is generated on first boot by
`camera-streamer-gencert.service` into `/etc/camera-streamer/tls/`.
Misconfiguration (one of cert/key missing, unreadable files) is a fatal
startup error by design — the service never silently falls back to
plaintext. Runtime dependency on target: `glib-networking` (GIO TLS
backend).

## Watchdog

A live PLAYING pipeline that pushes no buffer for 15 s is declared
stalled. A stalled camera does **not** restart the service: its mount is
disabled in place (new clients get 404, `get-status` reports it
unmounted) and the remaining cameras keep streaming — one dead sensor
must not take down the healthy one. The wedged pipeline is deliberately
left untouched (tearing down a stalled pipeline crashed on target); a
`reload` (SIGHUP) or service restart re-enables the camera if it comes
back. Only when *every* camera is dead does the service exit non-zero so
systemd's `Restart=on-failure` brings it back clean. Paused or idle
mounts are not stalls.

The service also runs under the systemd watchdog (`WatchdogSec`): the
main loop pets it periodically, so a wedged process is killed and
restarted by systemd.

### Runtime reconfiguration (USB ↔ ethernet switch)

The service re-reads its config on SIGHUP and re-serves with the new
settings — connected clients are dropped on purpose, and the bound address
is logged:

```sh
# on the device: restrict streaming to the wired network
sed -i 's/^listen=.*/listen=ethernet/' /etc/camera-streamer.conf
systemctl reload camera-streamer
```

`listen=all` (the default) serves USB and ethernet simultaneously, so
switching is only needed to *restrict* access to one network. If the chosen
interface has no IPv4 address yet (ethernet before DHCP), a failed reload
reverts to the previous config; a failed start exits and systemd retries
every 2 s until the address appears.

## Testing without camera hardware

`config/test-videotestsrc.conf` serves a videotestsrc pattern with software
H.264 encoding on `/cam0`; it runs on any machine with the standard GStreamer
plugin sets (base/good/ugly):

```sh
./build/camera-streamer config/test-videotestsrc.conf
# in another terminal:
ffplay rtsp://127.0.0.1:8554/cam0
```

## Playing the streams (host side)

With the device connected over USB (device IP 192.168.55.1):

The server offers TCP-interleaved RTP by default (see `transport=`), and
every client negotiates that automatically; forcing TCP client-side just
skips one round-trip:

```sh
# simplest
ffplay -rtsp_transport tcp rtsp://192.168.55.1:8554/cam0

# GStreamer, automatic pipeline
gst-launch-1.0 playbin uri=rtsp://192.168.55.1:8554/cam0

# GStreamer, explicit low-latency pipeline (H.265)
gst-launch-1.0 rtspsrc location=rtsp://192.168.55.1:8554/cam0 protocols=tcp \
    latency=100 ! rtph265depay ! h265parse ! avdec_h265 ! videoconvert ! autovideosink
```

Multiple clients can watch the same mount; the media factories are shared, so
the sensor is opened only once per camera.

## Service

`systemd/camera-streamer.service` is installed by `cmake --install` (unit dir
taken from `systemd.pc`, falling back to `<prefix>/lib/systemd/system`):

```sh
systemctl enable --now camera-streamer
journalctl -u camera-streamer -f
```

## Layout

The tree follows the folly/fboss conventions: CamelCase file names,
lowercase directories, everything in `namespace camera`, includes rooted
at `src/` (`#include "camera/rtsp/RtspServer.h"`).

```
CMakeLists.txt
src/camera/Main.cpp                Entry point: creates Application, runs it
src/camera/core/
  Application.{h,cpp}              Lifecycle, reload, signals, server wiring
  StreamController.h               IStreamController interface (RTSP abstraction)
  Watchdog.{h,cpp}                 systemd READY=1 / WATCHDOG=1 notifications
src/camera/config/
  Config.h                         Config + CameraConfig structs (DTOs)
  ConfigLoader.{h,cpp}             IConfigLoader interface + FileConfigLoader (GKeyFile)
src/camera/lib/
  net/NetworkResolver.{h,cpp}      listen= -> bind-address resolution
  v4l2/V4l2Device.{h,cpp}          IV4l2Device interface + V4l2Device impl (ioctl)
  v4l2/V4l2Factory.h               IV4l2DeviceFactory interface + create function
src/camera/pipeline/
  PipelineBuilder.{h,cpp}          GStreamer launch string fragments (shared)
  CameraSource.h                   ICameraSource interface (Strategy pattern)
  ArgusSource.{h,cpp}              Argus/ISP strategy (nvarguscamerasrc)
  V4l2Source.{h,cpp}               V4L2 strategy (v4l2src + VC driver controls)
  TestSource.{h,cpp}               Test pattern strategy (videotestsrc, no HW)
  SourceFactory.{h,cpp}            ISourceFactory interface + SourceFactory (Factory)
src/camera/rtsp/
  RtspServer.{h,cpp}               GstRTSPServer wrapper, implements IStreamController
  MountController.{h,cpp}          Per-camera mount state + frame tracking + watchdog
src/camera/control/
  ControlServer.{h,cpp}            TCP(+TLS) server + JSON-RPC envelope (transport only)
  ControlRegistry.{h,cpp}          Method name -> IControlHandler map (Registry)
  ControlHandler.h                 IControlHandler interface + ControlError/HandlerResult
  ControlContext.h                 ControlContext: dependencies passed to handlers
  JsonUtil.{h,cpp}                 JSON param extraction, result builders
  handlers/                        One handler class per protocol method
src/camera/discovery/
  DiscoveryServer.{h,cpp}          UDP discovery responder
src/camera/update/
  SwupdateClient.{h,cpp}           swupdate IPC: streaming install + progress polling
  UpdateServer.{h,cpp}             TCP(+TLS) .swu upload server (port 8557)
src/camera/folly/                  Vendored folly-mimic support library (see below)
../common/proto/Protocol.h         Ports/methods/error-code constants shared with host-ui
config/                            default + videotestsrc test configs
systemd/                           camera-streamer.service + first-boot TLS cert unit
```

### The `folly/` support layer

A small vendored mimic of the folly idioms the app uses — same names and
semantics, reimplemented on top of what is already on the device (GLib/GIO),
**not** a dependency on upstream folly:

```
src/camera/folly/
  Expected.h                       folly::Expected<T, Err> + makeUnexpected
  File.h                           Move-only RAII fd
  FileUtil.h                       readFull/writeFull (EINTR-safe)
  ScopeGuard.h                     SCOPE_EXIT
  Synchronized.h                   Mutex-guarded state (wlock/rlock/withWLock)
  logging/xlog.h                   XLOGF(LEVEL, fmt, ...) over g_log, [file:line] prefix
  io/async/EventBase.h             GMainLoop wrapper (loopForever, runInLoop, timers)
  io/async/AsyncServerSocket.*     GSocketService wrapper (bind/accept callbacks)
  io/async/SSLContext.*            GTlsServerConnection wrapper (TLS/mTLS)
  test/, io/async/test/            Standalone self-checks (assert-based, host-runnable)
```

House rules that follow from it: every fallible API returns
`folly::Expected<T, std::string>` (or `ControlError`) — no bool+out-param
signatures; raw fds live in `folly::File`; cross-thread state in
`folly::Synchronized`; cleanup via `SCOPE_EXIT`; logging via `XLOGF`.
GLib/GIO remains the engine underneath (GStreamer mandates the
GMainLoop); the folly layer is the API the app code sees.

### Design patterns

- **Strategy** (`ICameraSource`): argus/v4l2/test source behavior is
  encapsulated in separate strategy classes; adding a new source type is
  a new class + factory branch, no existing code changes (OCP).
- **Command + Registry** (`IControlHandler` + `ControlRegistry`): each
  control protocol method is a separate handler class; the dispatch
  if-else chain is replaced by a registry lookup. Adding a method is a
  new class + `register_handler` call (OCP).
- **Factory** (`ISourceFactory`, `IV4l2DeviceFactory`): creates strategy
  objects from config; mockable for unit tests.
- **Interface segregation** (`IStreamController`, `IConfigLoader`):
  control handlers depend on interfaces, not concrete classes, enabling
  unit testing with mocks (DIP).
