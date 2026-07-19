# Camera App — System Design

Embedded camera application on **Jetson Orin NX + VC MIPI dual IMX296**, built with
**Yocto** (existing `~/Projects/orin-nx` yb build), streaming video to a **Linux host
over USB**, with a host-side UI application.

---

> **Transport architecture (agreed 2026-07-19):** encryption and channel
> multiplexing sit above the transport, so USB and network carry the same
> encrypted record protocol and network mode becomes CSU-over-TCP rather than
> RTSP. See [docs/TRANSPORT-ARCHITECTURE.md](docs/TRANSPORT-ARCHITECTURE.md).
> Sections below that assume RTSP is the network path predate that decision.

## 1. System overview

```
┌─────────────────────────────────────────────┐         ┌──────────────────────────┐
│  Jetson Orin NX (Yocto image)               │   USB   │  Linux Host              │
│                                             │ (device │                          │
│  IMX296 #1 ──CSI(1-lane)──┐                 │  mode)  │  ┌────────────────────┐  │
│  IMX296 #2 ──CSI(1-lane)──┤                 │         │  │ Host UI app (Qt6 + │  │
│                            ▼                │  CDC-   │  │ GStreamer)         │  │
│  VC MIPI driver (patched L4T 35.x kernel)   │  NCM    │  │  - 2x video views  │  │
│  ┌───────────────────────────────────┐      │ gadget  │  │  - camera controls │  │
│  │ camera-streamer (C++ app)         │◄─────┼─────────┼─►│  - connection mgmt │  │
│  │  - capture: Argus/V4L2 via GST    │ RTSP │ 192.168.│  └────────────────────┘  │
│  │  - HW encode: nvv4l2h265enc      │ + TCP │ 55.0/24 │                          │
│  │  - RTSP server /cam0 /cam1        │ ctrl  │         │                          │
│  │  - control server (TCP/JSON±TLS)  │       │         │                          │
│  └───────────────────────────────────┘      │         │                          │
└─────────────────────────────────────────────┘         └──────────────────────────┘
```

Key facts driving the design:

- **IMX296** (VC MIPI): 1440×1080, global shutter, **1-lane MIPI CSI-2 only**, RAW10
  (RAW8/RAW12 possible), max ~60 fps, hardware trigger modes (external, sync, single,
  self…) exposed as V4L2 controls. Frame rate is *not* adjustable via V4L2 control.
- **Existing build** (`~/Projects/orin-nx/orin-nx.yml`, built with `yb`): OE-core +
  meta-tegra + tegra-demo-distro on branch **`scarthgap-l4t-r35.x`** → **JetPack 5,
  L4T r35.6.x, kernel 5.10**. Machine `p3768-0000-p3767-0000` (Orin NX module on the
  p3768 devkit carrier), distro `tegrademo`, image `demo-image-base`.
- **VC driver on L4T 35.x**: integrates by **patching the kernel source tree** (driver
  sources land in `kernel/nvidia/drivers/media/i2c`, device tree as **`.dtsi` includes**
  compiled into the flashed DTB). This is *different* from JP6/L4T 36.x, where the
  driver patches the separate `nvidia-oot` tree and DT ships as runtime `.dtbo`
  overlays. VC supports Orin NX on **35.6.0** and 36.2.0→36.5.0 — so the current
  r35.x branch is a supported base; **stay on it for milestone 1** and treat a JP6
  migration as a separate later task.
- **USB device mode on Jetson is USB 2.0 High-Speed** (~350–400 Mbit/s usable). RAW10
  1440×1080@60 is ~1.9 Gbit/s *per camera* → raw streaming over USB is impossible;
  **on-device H.264/H.265 hardware encoding (NVENC) is mandatory** for full-rate video.

---

## 2. Yocto build design

### 2.1 Layer stack (current + additions)

Already in `orin-nx.yml`: openembedded-core, bitbake, meta-openembedded (oe, python,
networking, filesystems), meta-tegra, meta-tegra-community, meta-virtualization, and
tegra-demo-distro's `meta-tegra-support`/`meta-tegrademo`/`meta-demo-ci` — all on
`scarthgap` / `scarthgap-l4t-r35.x`.

**Addition: one custom layer `meta-vc-camera`** (lives in this repo, added to the yb
manifest):

```yaml
# orin-nx.yml — add under repos:
  camera-app:
    path: repos/camera-app          # or a local path/symlink to ~/Projects/camera-app
    url: git@github.com:<you>/camera-app.git
    layers:
      yocto/meta-vc-camera:
```

**Critical version pin:** the kernel patches from `vc_mipi_nvidia` are per-L4T-version.
Confirm what the branch currently builds (`bitbake -e demo-image-base | grep
'^L4T_VERSION'`) and use the matching `vc_mipi_nvidia/patch/` set (35.6.0 for Orin NX;
minor deltas to 35.6.1/.2 are usually trivial but must be checked). Record the pair
(L4T version ⇄ VC patch set) in the layer and bump them only together.

### 2.2 `meta-vc-camera` layout

```
camera-app/
├── DESIGN.md
├── yocto/meta-vc-camera/
│   ├── conf/layer.conf
│   ├── recipes-kernel/linux/
│   │   ├── linux-tegra_%.bbappend           # VC driver + DT integration (see 2.3/2.4)
│   │   └── files/
│   │       ├── 0001-add-vc-mipi-driver.patch
│   │       ├── 0002-dt-dual-imx296-p3768.patch
│   │       └── vc-mipi.cfg                  # defconfig fragment
│   ├── recipes-core/usb-gadget/
│   │   └── usb-gadget-init.bb               # configfs NCM(+ACM) gadget + systemd unit
│   ├── recipes-apps/camera-streamer/
│   │   └── camera-streamer_git.bb           # our C++ app (CMake)
│   └── recipes-images/
│       └── camera-image.bb                  # demo-image-base + camera bits
├── embedded/                                # camera-streamer C++ source
├── host-ui/                                 # Qt6 host application
└── proto/                                   # shared control-protocol schema (M2)
```

### 2.3 Driver integration (`linux-tegra_%.bbappend`)

The VC repo is script-driven (`setup.sh` patches a JetPack source tree in place), which
doesn't map 1:1 to Yocto. **Vendor their changes as proper patches:**

1. One-time, on a scratch checkout of the L4T 35.6.x kernel sources (or a stock
   JetPack install), run VC's `setup.sh` flow for "NVIDIA Orin Nano DevKit / Orin NX +
   dual IMX296", then `git diff` the kernel tree into patch files. Much of it already
   exists ready-made under `vc_mipi_nvidia/patch/kernel_*_35.6.0/` plus
   `src/driver/*` (vc_mipi_core / vc_mipi_camera modules).
2. Ship in the bbappend:

```bitbake
# recipes-kernel/linux/linux-tegra_%.bbappend
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
SRC_URI += " \
    file://0001-add-vc-mipi-driver.patch \
    file://0002-dt-dual-imx296-p3768.patch \
    file://vc-mipi.cfg \
"
```

   The `.cfg` fragment enables the VC driver config symbols (module names per their
   Kconfig — verify in the patched tree) and anything they require in the tegra camera
   framework.
3. Regenerate the patch set only when bumping L4T.

### 2.4 Device tree — dual IMX296 on the p3768 carrier

On JP5 the DT is part of the kernel build and gets **flashed** with the BSP (no runtime
overlay step, unlike JP6). VC ships reference `.dtsi` files per carrier under
`src/devicetree/` — including the NVIDIA devkit carrier. The p3768 carrier has two
22-pin CSI connectors (CAM0 2-lane, CAM1 4-lane); each IMX296 uses 1 lane, so both fit.

Patch `0002` includes the VC dual-camera `.dtsi` from the carrier DTS
(`tegra234-p3768-0000+p3767-…`), with:

- two sensor nodes (distinct I²C bus/addresses per the VC adapter + carrier wiring),
- `tegra-camera-platform` module entries for both,
- VI/NVCSI/ISP port bindings, 1 data-lane each.

**Acceptance check** after flashing: `/dev/video0` + `/dev/video1` exist,
`v4l2-ctl -d /dev/video0 --list-formats-ext` shows RAW10 1440×1080, and
`v4l2-ctl --list-ctrls` shows the VC trigger/exposure controls.

Note: since the DTB is flashed, DT changes require a reflash (or at least DTB partition
update via `l4t_flash` tooling) — plan bring-up iterations accordingly.

### 2.5 USB gadget recipe

The devkit's USB-C port runs device mode. Stock L4T does this with
`nv-l4t-usb-device-mode`; tegra-demo-distro may or may not ship an equivalent — if
`demo-image-base` already brings up `l4tbr0`/RNDIS, adapt it; otherwise
`usb-gadget-init.bb` installs a configfs script + systemd oneshot creating a composite
gadget:

- **CDC-NCM** — USB network interface, static `192.168.55.1/24` on device, dnsmasq
  serving the host `192.168.55.100`. (RNDIS instead only if Windows hosts matter.)
- **ACM** — serial console on the same cable; free insurance during bring-up.

Why network-over-USB instead of a UVC (webcam) gadget:

| | CDC-NCM + RTSP (chosen) | UVC gadget |
|---|---|---|
| Dual streams | trivial (2 RTSP mounts) | 2 UVC functions, fragile tooling |
| Control channel | any TCP protocol | vendor UVC XUs — painful |
| Compressed video | native (H.264/H.265 RTP) | H.264 over UVC poorly supported |
| Host support | it's just ethernet | zero-install webcam (nice, but limiting) |
| Future (metadata, trigger, OTA) | free | dead end |

UVC gadget can still be added to the same composite gadget later if plug-and-play
webcam behavior is ever wanted.

### 2.6 Image recipe

```bitbake
# recipes-images/camera-image.bb — start from what already boots for you
require recipes-demo/images/demo-image-base.bb   # path per tegra-demo-distro

IMAGE_INSTALL += " \
    gstreamer1.0-rtsp-server \
    gstreamer1.0-plugins-nvarguscamerasrc gstreamer1.0-plugins-nvvidconv \
    gstreamer1.0-plugins-nvvideo4linux2 gstreamer1.0-plugins-nvvideosinks \
    v4l-utils dnsmasq \
    usb-gadget-init camera-streamer \
"
```

(The NVIDIA GStreamer element recipe names vary slightly per meta-tegra branch — take
the authoritative list from tegra-demo-distro's `demo-image-full`, which includes the
full multimedia stack.)

Build stays exactly your current flow: `yb build orin-nx.yml` with
`target: camera-image`. Flash with the meta-tegra flash artifacts as today.

---

## 3. Video pipeline design

Two capture paths exist on Jetson; support both behind one interface, choose per config:

1. **Argus/ISP path** (`nvarguscamerasrc`) — RAW through the Tegra ISP: debayer (needed
   for **color** IMX296C), 3A, NV12 in NVMM (zero-copy into NVENC). **Default for
   color modules.**
2. **Pure V4L2 path** (`v4l2src`) — RAW10 straight from VI. Right choice for **mono**
   IMX296 (no debayer needed) and for trigger-synchronized machine-vision capture where
   deterministic frames matter more than 3A. Needs format conversion before NVENC.

Milestone-1 pipeline (per camera, on device):

```
nvarguscamerasrc sensor-id=0 ! 'video/x-raw(memory:NVMM),width=1440,height=1080,framerate=60/1'
  ! nvv4l2h265enc bitrate=8000000 insert-sps-pps=true idrinterval=30
  ! h265parse ! rtph265pay
```

wrapped in **gst-rtsp-server** with mounts `rtsp://192.168.55.1:8554/cam0` and `/cam1`.

Bandwidth check: 2 × 8 Mbit/s H.265 ≪ ~350 Mbit/s usable USB2 — big headroom (2 ×
~50 Mbit/s near-lossless is still fine).

Smoke test with zero custom code (bring-up checkpoint before any app work):
device: `gst-launch-1.0 nvarguscamerasrc ! … ! udpsink host=192.168.55.100 port=5000`,
host: `gst-launch-1.0 udpsrc port=5000 ! … ! autovideosink`.

---

## 4. Embedded application (`camera-streamer`, C++)

C++17, CMake, systemd service. A supervised GStreamer graph plus the servers
around it (the tree below is the shipped structure — folly/fboss-style naming,
`namespace camera`; see `embedded/README.md` for the full layout):

```
camera-streamer (embedded/src/camera/)
├── config/       INI (GKeyFile): sensor mode, path (argus|v4l2), bitrate, ports, TLS
├── pipeline/     CameraSource strategies: ArgusSource | V4l2Source | TestSource
├── rtsp/         RtspServer + per-camera MountController (gst-rtsp-server, /cam0 /cam1)
├── control/      ControlServer — TCP(+TLS), newline-delimited JSON  [done, M2]
│                   exposure/gain/trigger/ISP/zoom/sync, status, reload, reboot
├── update/       UpdateServer (.swu upload, port 8557) + SwupdateClient (swupdate IPC)
├── discovery/    UDP discovery responder
├── core/         Application lifecycle + systemd watchdog; stall watchdog policy:
│                   a stalled camera is disabled in place (others keep streaming),
│                   exit-for-restart only when every camera is dead
└── folly/        vendored folly-mimic support layer (Expected, File, Synchronized,
                    SCOPE_EXIT, XLOGF, GIO-backed EventBase/AsyncServerSocket/SSLContext)
```

Cross-cutting: `common/proto/Protocol.h` holds the protocol constants shared
with the host UI; the control and update channels take optional TLS/mTLS
(`[server] tls-cert/tls-key/tls-ca`, self-signed device cert generated on
first boot — the "secure USB" story: only host software holding a
CA-signed client cert can command the device).

The control channel shipped as **newline-delimited JSON over TCP (port
8555)** instead of protobuf — same wire role, but no codegen, no extra host
dependency (Qt ships JSON, json-glib is in oe-core), and debuggable with
`nc`. The schema lives in `proto/PROTOCOL.md` (shared by both sides), and
the door to protobuf stays open if the protocol ever outgrows JSON.

Design notes:

- **Transport is interface-agnostic**: the RTSP server binds a configurable address
  (`[server] listen = all | usb | ethernet | <ip/iface>`), so the same streams serve
  over the USB gadget network, the wired GigE port, or both at once (`all`, default).
  SIGHUP (`systemctl reload`) re-reads the config and rebinds at runtime — Ethernet
  also lifts the USB2 bandwidth ceiling when a wired link is available.
- Use GStreamer as the media backbone rather than hand-rolled Argus/V4L2 + NVENC API:
  the NVMM zero-copy path, RTSP, and pipeline introspection come free; the app's value
  is configuration, control, and supervision.
- The VC trigger controls are the hook for **hardware-synchronized dual capture** later
  (external trigger fanned out to both sensors). Make trigger mode a first-class
  setting in `control/` from day one.
- Recipe: `inherit cmake pkgconfig systemd`, `DEPENDS = "gstreamer1.0
  gstreamer1.0-plugins-base gstreamer1.0-rtsp-server"`, service enabled by default.

---

## 5. Host UI application

- **Qt 6 + QML**, video via explicit GStreamer pipelines
  (`rtspsrc latency=0 ! decodebin ! glsinkbin`/`qml6glsink`) so latency is controllable
  and pipeline knowledge is shared with the device side.
- Two video panes (cam0/cam1), connection panel (fixed 192.168.55.1 for M1), controls
  pane speaking the JSON-over-TCP protocol (`proto/PROTOCOL.md`, M2): status polling,
  exposure/gain per camera, hardware trigger mode.
- Plain desktop app in `host-ui/`, not part of the Yocto build; shares `proto/` with
  the embedded app.

---

## 6. Milestones

**M1 — video stream on host** ✅ *verified on hardware 2026-07-12*
1. `meta-vc-camera` added to `orin-nx.yml`; `camera-image` builds & boots (no camera
   bits yet — pure plumbing checkpoint).
2. VC driver + dual-IMX296 DT patches in `linux-tegra` bbappend → `/dev/video0`/`1`
   with correct formats/controls after reflash.
3. USB NCM gadget up; host pings 192.168.55.1; ACM console works.
4. `camera-streamer` v0: RTSP server, one camera, H.265 60 fps; host plays it with
   `ffplay rtsp://192.168.55.1:8554/cam0` (no UI needed to close M1).

**M2 — dual streams + host UI** ✅ *verified on hardware 2026-07-12*: dual
concurrent 1440×1080@60 H.265 streams, Qt UI dual view + control panel,
JSON/TCP control channel (exposure/gain/trigger/ISP/zoom, status with live AE
readback, reload), pipeline stall watchdog, OTA updates via swupdate (A/B).

**M3 — productization**: hardware-triggered sync capture, frame metadata (timestamp,
sequence), device discovery, OTA (RAUC/Mender on meta-tegra), factory flash flow.
*Bring-up finding for sync capture*: the pure-V4L2 path needs a DT change first —
the VC template declares every mode `bayer/rggb`, so the mono IMX296 only exposes
RG10 (no GREY/Y10), which `v4l2src` cannot consume; both cameras currently stream
via Argus (mono debayers to a gray image, minor artifacts). For triggered capture,
switch cam1's DT mode to gray (`pixel_t`/Y10) and revisit the capture element.
*Software side implemented*: `set-sync`/`fire-trigger` orchestration and per-frame
`last_frame` metadata in the control protocol, UDP discovery (port 8556) with a
Discover button in the host UI, OTA via swupdate (A/B rootfs, streaming `.swu`
upload on port 8557 from the host UI, reboot method), optional TLS/mTLS on the
control/update channels. *Still open*: on-target verification with real
trigger wiring, factory flash flow.

---

## 7. Risks & open questions

| Risk | Mitigation |
|---|---|
| Exact L4T minor (35.6.0 vs .1/.2) vs VC patch set | Check `L4T_VERSION` in the build; diff-review VC patches against the actual kernel tree; pin the pair in the layer. |
| Color IMX296C image quality via Argus (ISP tuning) | *Partly resolved on target*: the pink haze was the unsubtracted 60-LSB sensor pedestal — a measured black-level `camera_overrides.isp` now ships via `vc-isp-tuning`. Runtime ISP controls shipped (`set-isp`). Remaining: WB/CCM calibration with a ColorChecker (`tools/isp-tuning/`), and check the lens for an IR-cut filter before daylight use. |
| Host networks that drop unsolicited inbound UDP (observed: no RTP received while the server sent perfectly) | RTSP serves TCP-interleaved by default (`[server] transport`); clients negotiate it automatically. |
| USB2-only device mode (~350 Mbit/s) | HW encode on device (designed in). If the host ever needs raw frames, use GigE or reduced fps/ROI. |
| DTB is flashed on JP5 → slow DT iteration | Script the DTB-only reflash path during bring-up. |
| Future JP6 migration changes integration model (nvidia-oot + .dtbo overlays) | Isolated in `meta-vc-camera` (one bbappend + DT recipe swap); app and gadget layers unaffected. |
| Which IMX296 variant (mono vs color)? VC adapter wiring on the two devkit CSI ports? | **Open — determines Argus-vs-V4L2 default and the exact DT. Confirm before writing patch 0002.** |
