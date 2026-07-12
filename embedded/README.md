# camera-streamer

RTSP streaming service for the Jetson Orin NX camera device (milestones 1-2,
see `../DESIGN.md`). Serves one hardware-encoded H.265/H.264 stream per
camera, plus a TCP control channel for runtime camera settings:

```
rtsp://192.168.55.1:8554/cam0
rtsp://192.168.55.1:8554/cam1
tcp://192.168.55.1:8555        (control protocol, ../proto/PROTOCOL.md)
```

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
everything else the VC driver exposes, and `reload`. A UDP responder on
port 8556 answers `{"method":"discover"}` broadcasts so the host UI can
find devices. The TCP side is plain enough to drive by hand:

```sh
printf '{"id":1,"method":"set-exposure","params":{"camera":0,"us":5000}}\n' \
    | nc -q1 192.168.55.1 8555
```

Exposure/gain on the `argus` path pin the matching `nvarguscamerasrc` 3A
range on the live pipeline (and seed future pipelines); on the `v4l2` path
they go straight to the VC driver's V4L2 controls. Hardware trigger modes
are v4l2-only — Argus owns the sensor timing.

## Watchdog

A live PLAYING pipeline that pushes no buffer for 15 s is declared stalled:
the service logs a critical message and exits non-zero, and systemd's
`Restart=on-failure` brings it back with a clean pipeline. Paused or idle
mounts are not stalls.

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

```
CMakeLists.txt
src/main.cpp                       Entry point: creates Application, runs it
src/core/
  application.{h,cpp}              Application class: lifecycle, reload, signals
  stream_controller.h              IStreamController interface (RTSP abstraction)
src/config/
  config.h                         Config + CameraConfig structs (DTOs)
  config_loader.{h,cpp}            IConfigLoader interface + FileConfigLoader (GKeyFile)
src/net/
  network_resolver.{h,cpp}         listen= -> bind-address resolution
src/v4l2/
  v4l2_device.{h,cpp}              IV4l2Device interface + V4l2Device impl (ioctl)
  v4l2_factory.h                   IV4l2DeviceFactory interface + create function
src/pipeline/
  pipeline_builder.{h,cpp}         GStreamer launch string fragments (shared)
  camera_source.h                  ICameraSource interface (Strategy pattern)
  argus_source.{h,cpp}             Argus/ISP strategy (nvarguscamerasrc)
  v4l2_source.{h,cpp}              V4L2 strategy (v4l2src + VC driver controls)
  test_source.{h,cpp}              Test pattern strategy (videotestsrc, no HW)
  source_factory.{h,cpp}           ISourceFactory interface + SourceFactory (Factory)
src/rtsp/
  rtsp_server.{h,cpp}              GstRTSPServer wrapper, implements IStreamController
  mount_controller.{h,cpp}         Per-camera mount state + frame tracking + watchdog
src/control/
  control_server.{h,cpp}           TCP server + JSON-RPC envelope (transport only)
  control_registry.{h,cpp}         Method name -> IControlHandler map (Registry)
  control_handler.h                IControlHandler interface (Command pattern)
  control_context.h                ControlContext: dependencies passed to handlers
  json_util.{h,cpp}                JSON param extraction, result builders, error codes
  handlers/
    register_handlers.{h,cpp}      Registers all handlers into the registry
    system_handlers.{h,cpp}        ping, reload
    status_handler.{h,cpp}         get-status, get-config
    exposure_handler.{h,cpp}       set-exposure, set-gain
    trigger_handler.{h,cpp}        set-trigger, fire-trigger, set-sync
    isp_handler.{h,cpp}            set-isp
    zoom_handler.{h,cpp}           set-zoom
    v4l2_control_handler.{h,cpp}   list-controls, get-control, set-control
src/discovery/
  discovery_server.{h,cpp}         UDP discovery responder
config/                            default + videotestsrc test configs
systemd/                           service unit
```

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
