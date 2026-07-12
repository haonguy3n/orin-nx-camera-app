# camera-app — dual IMX296 streaming for Jetson Orin NX

C++ camera device software for a **Jetson Orin NX + VC MIPI dual IMX296**
(one color IMX296C, one mono IMX296), built with Yocto, streaming
hardware-encoded H.265 over RTSP to a Linux host — over the USB-C cable
(CDC-NCM network gadget) or ethernet — with a Qt host application for
viewing and camera control.

```
rtsp://192.168.55.1:8554/cam0   color IMX296C (Argus/ISP)
rtsp://192.168.55.1:8554/cam1   mono IMX296  (Argus for now, see DESIGN M3)
tcp://192.168.55.1:8555         JSON control protocol (proto/PROTOCOL.md)
udp  192.168.55.1:8556          discovery responder
```

**Status**: M1 (streaming) and M2 (dual streams, host UI, control channel)
verified on hardware — dual concurrent 1440×1080@60 H.265. M3 in progress:
discovery/trigger/zoom/frame-metadata shipped; hardware-triggered sync
capture, color calibration and factory flash still open. OTA updates work
via swupdate (A/B rootfs). See `DESIGN.md` for architecture and milestones.

## Layout

| Path | What |
|---|---|
| `DESIGN.md` | architecture, decisions, milestones, risks |
| `embedded/` | `camera-streamer`: GStreamer RTSP + control/discovery servers (C++17) |
| `host-ui/` | `camera-viewer`: Qt6 dual-pane viewer + camera control panel (`./build.sh run`) |
| `proto/PROTOCOL.md` | the JSON/TCP control protocol both sides implement |
| `yocto/meta-vc-camera/` | Yocto layer: VC kernel driver + DT, USB gadget, ISP tuning, image |
| `tools/isp-tuning/` | DIY ISP calibration (black level, CCM) without NVIDIA's NDA tools |

## Quickstart

**Device image** (build machine with the `~/Projects/orin-nx` yb project —
see `yocto/README.md` for how the layer hooks in):

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
ffplay -rtsp_transport tcp rtsp://192.168.55.1:8554/cam0
printf '{"id":1,"method":"get-status"}\n' | nc -q1 192.168.55.1 8555
```

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
