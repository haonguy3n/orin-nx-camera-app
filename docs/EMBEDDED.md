# camera-streamer

Camera service for the Jetson Orin NX device (see `DESIGN.md`,
`TRANSPORT-ARCHITECTURE.md`). Serves one hardware-encoded H.265/H.264
stream per camera plus control, firmware update and face-detection
metadata, over **one of two transports** (`[server] transports`):

`transports=usb` (default) — everything is multiplexed into an
authenticated, encrypted session on the FunctionFS USB endpoints. No
TCP/UDP socket is bound except the recovery `.swu` listener:

```
usb endpoints ep1/ep2          video + control + update + detection boxes
tcp://192.168.55.1:8557        recovery .swu upload (the one bound socket)
```

`transports=network` — classic RTSP + TCP for interop:

```
rtsp://<device>:8554/cam0      (and /cam1)
tcp://<device>:8555            (control protocol, PROTOCOL.md)
udp  <device>:8556             (discovery responder)
tcp://<device>:8557            (OTA .swu upload -> swupdate)
```

In network mode the control and update channels can be wrapped in
TLS/mTLS — see "TLS (network mode)" below. Port and method constants
shared with the host UI live in `../common/proto/Protocol.h`.

## Build

Dependencies: CMake >= 3.16, a C++20 compiler, and dev packages for
`gstreamer-1.0`, `gstreamer-rtsp-server-1.0` and `json-glib-1.0`
(Debian/Ubuntu: `libgstreamer1.0-dev libgstrtspserver-1.0-dev
libjson-glib-dev`; Arch: `gstreamer` `gst-rtsp-server` `json-glib`).
The secure USB transport and face detection (`ENABLE_SECURE_USB`, on by
default) additionally need OpenSSL and OpenCV (core/imgproc/objdetect/dnn;
the CUDA DNN backend on target); `-DENABLE_SECURE_USB=OFF` builds the
network-only service without them.

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

Keys tagged [network] are ignored under `transports=usb` and vice versa
(the shipped config file carries the same tags):

| Section | Key | Default | Meaning |
|---|---|---|---|
| `[server]` | `transports` | `usb` | which transport carries video/control/update: `usb` (secure USB endpoints, binds no socket but the recovery listener) or `network` (RTSP/TCP). **One at a time** — Argus permits a single consumer per camera; anything else is rejected and treated as `usb` |
| | `recovery-update` | `true` | [usb] `.swu` upload listener on the CDC-NCM address (`192.168.55.1:<update-port>`) — the way firmware gets pushed when the secure USB transport itself is broken |
| | `port` | `8554` | [network] RTSP listen port |
| | `listen` | `all` | [network] network to serve on: `all` (USB + ethernet at once), `usb` (usb0 only), `ethernet` (eth0 only), or an explicit IPv4 address / interface name |
| | `transport` | `tcp` | [network] RTP transport offered: `tcp` (interleaved, firewall-proof), `udp`, or `all` (client picks) |
| | `control-port` | `8555` | [network] TCP control protocol port, same bind address as RTSP; `0` disables. Under `transports=usb` control is dispatched in-process, no listener |
| | `discovery-port` | `8556` | [network] UDP discovery responder (always 0.0.0.0); `0` disables |
| | `update-port` | `8557` | [both] OTA `.swu` upload port (binary, streamed into swupdate IPC); `0` disables. Normal listener in network mode; recovery-only in usb mode |
| | `tls-cert` / `tls-key` | unset | [network] PEM certificate + key; setting both enables TLS on the control and update ports. Setting only one, or an unreadable file, is a **fatal startup error** — no silent plaintext fallback. The same cert signs the secure-USB handshake |
| | `tls-ca` | unset | [network] additionally **require** a client certificate signed by this CA (mTLS); only authorized host software can command the device |
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
| `[detect]` | `model` | `/usr/share/camera-streamer/face_detection_yunet.onnx` | YuNet ONNX model path. The file's presence **is** the enable switch — no readable model, no detection |
| | `width` | `320` | detector working width; height follows the camera's aspect ratio (forcing a square squashes faces and YuNet misses them) |
| | `score` | `0.45` | minimum detection confidence 0..1 |
| | `fps` | `10` | detections per second, deliberately far below capture rate (measured: pacing to 10 took GR3D 36–41% → 25–28%); `0` = unlimited |

The final GStreamer launch string for every mount is logged at startup —
useful for reproducing issues with plain `gst-launch-1.0`.

## Control protocol

`PROTOCOL.md` defines the newline-delimited JSON protocol: `get-status`
(per-camera streaming state, frame counters, live AE exposure/gain
readback, and `last_frame` capture metadata for cross-camera sync
checks), `set-exposure` / `set-gain` / `set-trigger`, `set-isp` (runtime
WB/saturation/TNR/EE), `set-zoom` (digital zoom + pan), `set-sync` (all
cameras to external trigger) / `fire-trigger` (software single trigger),
generic `list-controls`/`get-control`/`set-control` for everything else
the VC driver exposes, `get-metrics` (CPU/GPU/VIC/NVENC load and
temperatures), `snapshot` (write a PPM off the detection branch, for ISP
judgement), `reload`, `reboot`, and `get-update-status` (swupdate
install progress). In network mode detection boxes are pushed as
`{"event":"faces",...}` lines on the same connection.

The same requests, byte for byte, travel both transports: a TCP listener
on port 8555 in network mode, in-process dispatch off the secure USB
control channel in usb mode (marshalled onto the GLib main loop — the
handlers mutate config and live pipeline elements).

In network mode a UDP responder on port 8556 answers
`{"method":"discover"}` broadcasts so the host UI can find devices, and
port 8557 accepts `.swu` firmware uploads (streamed straight into
swupdate's IPC socket, no temp file — see PROTOCOL.md "OTA firmware
update"). The TCP side is plain enough to drive by hand:

```sh
printf '{"id":1,"method":"set-exposure","params":{"camera":0,"us":5000}}\n' \
    | nc -q1 192.168.55.1 8555
```

Exposure/gain on the `argus` path pin the matching `nvarguscamerasrc` 3A
range on the live pipeline (and seed future pipelines); on the `v4l2` path
they go straight to the VC driver's V4L2 controls. Hardware trigger modes
are v4l2-only — Argus owns the sensor timing.

## TLS (network mode)

The channels that *command* the device (control 8555, update 8557) can be
TLS-wrapped; RTSP and discovery are unaffected. Irrelevant under
`transports=usb`, where the whole session is already authenticated and
encrypted end to end — but the same device certificate signs the secure
USB handshake, so first-boot generation serves both. Enable by setting
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

## Playing the streams (host side, network mode)

Under `transports=usb` there is no RTSP — the viewer's secure USB bridge
is the only consumer. With `transports=network` and the device connected
over USB (device IP 192.168.55.1):

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
  Application.{h,cpp}              Lifecycle, reload, signals, per-transport wiring
  StreamController.h               IStreamController interface
  UsbAwareStreamController.h       Routes runtime settings to whichever transport serves
  Watchdog.{h,cpp}                 systemd READY=1 / WATCHDOG=1 notifications
src/camera/config/
  Config.h                         Config + CameraConfig structs (DTOs)
  ConfigLoader.{h,cpp}             IConfigLoader interface + FileConfigLoader (GKeyFile)
src/camera/lib/
  net/NetworkResolver.{h,cpp}      listen= -> bind-address resolution
  v4l2/V4l2Device.{h,cpp}          IV4l2Device interface + V4l2Device impl (ioctl)
  v4l2/V4l2Factory.h               IV4l2DeviceFactory interface + create function
  sys/ResourceMonitor.{h,cpp}      CPU/GPU/VIC/NVENC load + temps (get-metrics)
src/camera/media/
  Element/Bin/Pipeline.{h,cpp}     Programmatic GStreamer object model
  CameraPipeline.{h,cpp}           One per sensor: lifecycle, video + raw fanout
src/camera/secure/
  SecureUsbServer.{h,cpp}          usb transport: session, channel mux, sinks
  FfsGadget.{h,cpp}                FunctionFS endpoint lifecycle (ffs.secure)
  SecureRecord/KeySchedule.{h,cpp} Record layer + HKDF key derivation
src/camera/detect/
  FaceDetector.{h,cpp}             YuNet on OpenCV/CUDA + to_meta_json
  MetaSink.h                       IMetaSink: per-transport box delivery
  Snapshot.{h,cpp}                 PPM snapshot off the detection branch
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
                                   + network-mode detection thread
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
src/camera/base/                  Project base library (vendored folly-API mimics) (see below)
../common/proto/Protocol.h         Ports/methods/error-code constants shared with host-ui
../common/secure/SecureUsbContext.* Reusable TLS-style secure context/session API
config/                            default + videotestsrc test configs
systemd/                           camera-streamer.service + first-boot TLS cert unit
```

### The `folly/` support layer

A small vendored mimic of the folly idioms the app uses — same names and
semantics, reimplemented on top of what is already on the device (GLib/GIO),
**not** a dependency on upstream folly:

```
src/camera/base/
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
