# camera-app — dual IMX296 streaming for Jetson Orin NX

C++ camera device software for a **Jetson Orin NX + VC MIPI dual IMX296**,
built with Yocto, streaming hardware-encoded H.265 to a Linux host over the
USB-C cable, with on-device face detection and a Qt host application for
viewing and camera control.

**One transport serves video at a time**, chosen by `[server] transports`
(Argus permits a single consumer per camera). See
[docs/TRANSPORT-ARCHITECTURE.md](docs/TRANSPORT-ARCHITECTURE.md).

`transports=usb` — the default. Everything rides the USB endpoints inside an
authenticated, encrypted session (ECDHE-P256 + ChaCha20-Poly1305), multiplexed
into channels: video, control, firmware update, and detection metadata. **No
TCP or UDP socket is bound at all**, except the recovery listener below.

```
usb endpoints ep1/ep2       video + control + update + face-detection boxes
tcp://192.168.55.1:8557     recovery .swu upload (the one bound socket)
```

`transports=network` — RTSP for interop with VLC/ffmpeg/NVRs. Video is **not**
encrypted on this path; detection boxes arrive as control events instead of on
the metadata channel.

```
rtsp://<device>:8554/cam0   H.265, colour IMX296C (Argus/ISP)
tcp://<device>:8555         JSON control protocol + pushed events
udp  <device>:8556          discovery responder
tcp://<device>:8557         OTA .swu upload -> swupdate (A/B rootfs)
```

Face detection (YuNet on the GPU via OpenCV/CUDA) runs on **both** transports;
the host draws the same boxes either way.

**Status**: streaming, host UI, control channel, OTA (swupdate, A/B rootfs)
and on-device face detection verified on hardware. Both transports work and
both carry detection.

Open: colour calibration is partly done — the optical-black pedestal fix
removed the magenta cast, but the current single value over-subtracts blue, so
a per-channel bias is still needed. Hardware-triggered sync capture and factory
flash remain. **cam1 is currently disabled**: the second sensor is unplugged,
and it did not enumerate before that (Argus saw one sensor). See
`docs/DESIGN.md` for architecture and milestones.

## Layout

All documentation lives in `docs/` (recipe-local notes stay next to their
recipes):

| Path | What |
|---|---|
| `docs/DESIGN.md` | architecture, decisions, milestones, risks |
| `docs/TRANSPORT-ARCHITECTURE.md` | the two transport modes: class→file map, threading |
| `docs/PROTOCOL.md` | the JSON control protocol both sides implement |
| `docs/EMBEDDED.md` | `camera-streamer` reference: build, config keys, service |
| `docs/HOST-UI.md` | `camera-viewer` reference: build, transports, control panel |
| `docs/YOCTO.md` | how the layer hooks into the build, flashing, OTA |
| `docs/ISP-TUNING.md` | DIY ISP calibration (black level, CCM) without NVIDIA's NDA tools |
| `embedded/` | `camera-streamer`: secure-USB + RTSP transports, control/discovery/update servers, face detection (C++20, folly-style) |
| `host-ui/` | `camera-viewer`: Qt6 dual-pane viewer + camera control panel (`./build.sh run`) |
| `common/` | code built into both sides: `proto/Protocol.h` constants, `secure/` handshake + wire crypto |
| `yocto/meta-vc-camera/` | Yocto layer: VC kernel driver + DT, USB gadget, ISP tuning, image |
| `tools/isp-tuning/` | the calibration scripts themselves |

## Quickstart

**Device image** (build machine with the `~/Projects/orin-nx` yb project —
see `docs/YOCTO.md` for how the layer hooks in):

```sh
cd ~/Projects/orin-nx && yb build orin-nx.yml
# first install: extract camera-image tegraflash tarball, sudo ./initrd-flash
# updates:      scp camera-image-*.swu root@<device>:/tmp/ && ssh root@<device> 'swupdate -i /tmp/*.swu && reboot'
```

**Host viewer**:

```sh
cd host-ui && ./build.sh run     # needs cmake + qt6-multimedia
```

Hit **Discover** (or type the device IP) → **Connect**. Exposure, gain,
trigger, digital zoom and ISP controls live in the right-hand panel.

**No UI needed** for a quick look:

```sh
# network mode only; usb mode publishes no RTSP
ffplay -rtsp_transport tcp rtsp://192.168.55.1:8554/cam0
printf '{"id":1,"method":"get-status"}\n' | nc -q1 192.168.55.1 8555
```

## Secure USB transport

The native C++ secure-transport protocol uses an ephemeral P-256 ECDH
exchange, the device's pinned certificate to sign the handshake, HKDF-SHA256
directional keys, and ChaCha20-Poly1305 records. The records ride dedicated
FunctionFS endpoints (`ffs.secure` in the composite gadget) on the device and
libusb in the viewer — verified on hardware. The CDC-NCM network interface
stays up in both modes for ssh and the recovery `.swu` listener; switching to
the RTSP/control path is a config change (`transports=network` + reload), not
an image rebuild.

## Bring-up lessons (why some defaults look unusual)

- RTSP serves **TCP-interleaved** by default: some hosts statefully drop
  inbound UDP and the loss is silent (`transport=` in the config restores UDP).
- The mono camera also runs through Argus: the device tree declares both
  sensors bayer, so pure-V4L2 grey capture needs a DT change first (M3).
- `camera_overrides.isp` ships a measured black-level fix — without it the
  color image has a pink haze (unsubtracted sensor pedestal).
- The `queue` elements in the pipelines are load-bearing (NVMM buffer
  starvation inside gst-rtsp-server), as is every RDEPENDS GStreamer plugin
  (each missing one produced a distinct on-target failure).
