# camera-streamer

RTSP streaming service for the Jetson Orin NX camera device (milestone 1, see
`../DESIGN.md`). Serves one hardware-encoded H.265/H.264 stream per camera:

```
rtsp://192.168.55.1:8554/cam0
rtsp://192.168.55.1:8554/cam1
```

## Build

Dependencies: CMake >= 3.16, a C++17 compiler, and dev packages for
`gstreamer-1.0` and `gstreamer-rtsp-server-1.0` (Debian/Ubuntu:
`libgstreamer1.0-dev libgstrtspserver-1.0-dev`; Arch: `gstreamer`
`gst-rtsp-server`).

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
| `[cam0]`/`[cam1]` | `enabled` | `true` | serve this camera at `/cam0`/`/cam1` |
| | `source` | `argus` | `argus` (nvarguscamerasrc/ISP), `v4l2` (v4l2src, best-effort in M1), `test` (videotestsrc + software encoder, no hardware needed) |
| | `sensor-id` | `0` / `1` | argus only: nvarguscamerasrc sensor-id |
| | `device` | `/dev/video0` / `/dev/video1` | v4l2 only: capture device |
| | `width` | `1440` | capture width |
| | `height` | `1080` | capture height |
| | `framerate` | `60` | frames per second (integer) |
| | `codec` | `h265` | `h265` or `h264` |
| | `bitrate` | `8000000` | encoder target, bit/s |

The final GStreamer launch string for every mount is logged at startup —
useful for reproducing issues with plain `gst-launch-1.0`.

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

```sh
# simplest
ffplay rtsp://192.168.55.1:8554/cam0

# GStreamer, automatic pipeline
gst-launch-1.0 playbin uri=rtsp://192.168.55.1:8554/cam0

# GStreamer, explicit low-latency pipeline (H.265)
gst-launch-1.0 rtspsrc location=rtsp://192.168.55.1:8554/cam0 latency=100 ! \
    rtph265depay ! h265parse ! avdec_h265 ! videoconvert ! autovideosink
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
src/main.cpp            App struct (config + RTSP server + main loop), signals
src/config.{h,cpp}      Config structs + GKeyFile INI loader
src/rtsp_server.{h,cpp} GstRTSPServer wrapper, launch-string construction
config/                 default + videotestsrc test configs
systemd/                service unit
```

Milestone 2 (TCP control server for exposure/gain/trigger) plugs in as another
member of `App` next to `rtsp`; no restructuring needed.
