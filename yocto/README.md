# yocto/ — meta-vc-camera

Yocto layer for the camera device image (Jetson Orin NX, dual VC MIPI IMX296).
Builds on top of the existing `~/Projects/orin-nx` build (yb/kas, OE-core +
meta-tegra + tegra-demo-distro, branch `scarthgap` / `scarthgap-l4t-r35.x`,
L4T r35.6.4 / JetPack 5, machine `p3768-0000-p3767-0000`, distro `tegrademo`).

## Building

```sh
cd ~/Projects/orin-nx
yb build camera-image        # positional target overrides the yml's default
```

The yml's default `target:` is still `demo-image-base`, so a plain `yb build`
keeps building the stock image.

## How the layer is hooked in

This repo is not cloned into the build project — it is bind-linked in:

- `~/Projects/orin-nx/repos/camera-app` is a **symlink** to
  `~/Projects/camera-app` (this working tree). Edits here are picked up by
  the next build with no commit/fetch step.
- `orin-nx.yml` has a **url-less repo entry** `camera-app` whose layer path is
  `repos/camera-app/yocto/meta-vc-camera`. yb (like kas) treats a url-less
  repo as the project directory itself and skips git checkout for it, so the
  layer path resolves through the symlink and nothing is ever fetched.
- The `yb: mounts:` entry for `/home/hao/Projects/camera-app` is only relevant
  if the build is ever switched to a container (`yb: version:`/`image:`);
  today the build runs natively on the host and the mount list is unused.
- `camera-streamer.bb` builds `../embedded` via **externalsrc** (path derived
  from the layer location), so the app also builds straight from this tree.

Once the repo has a remote and commits, the yml entry can become a normal
`url:`/`branch:` repo with `layers: yocto/meta-vc-camera:` and the symlink can
go away.

## What the layer adds

- `recipes-core/usb-gadget/usb-gadget-init.bb` — configfs composite USB
  gadget (systemd oneshot `usb-gadget.service`): CDC-NCM network function +
  ACM serial function, plus dnsmasq config to serve the host its address and
  a getty on the gadget serial port.
- `recipes-apps/camera-streamer/camera-streamer.bb` — the C++ RTSP streaming
  app from `../embedded` (CMake, gstreamer + gst-rtsp-server, installs its
  own `camera-streamer.service` and `/etc/camera-streamer.conf`).
- `recipes-images/camera-image.bb` — `demo-image-base` + NVIDIA GStreamer
  elements (`gstreamer1.0-plugins-nvarguscamerasrc/-nvvidconv/
  -nvvideo4linux2/-nvvideosinks`), `gstreamer1.0-rtsp-server`, `v4l-utils`,
  `dnsmasq`, `usb-gadget-init`, `camera-streamer`.
- `recipes-kernel/` — VC MIPI driver + device-tree integration (separate
  workstream; see DESIGN.md 2.3/2.4).

## Flashing

Same flow as the stock demo image — meta-tegra's tegraflash artifacts. After
the build:

```sh
cd ~/Projects/orin-nx/build/tmp/deploy/images/p3768-0000-p3767-0000
mkdir flash && cd flash
tar xf ../camera-image-p3768-0000-p3767-0000.rootfs.tegraflash.tar.gz
# device in recovery mode (hold REC while powering on), USB to the flashing port
sudo ./doflash.sh          # full flash
# or ./initrd-flash for the initrd-based flow
```

See meta-tegra's scarthgap docs for details/variants:
<https://github.com/OE4T/meta-tegra/wiki/Flashing-the-Jetson-Dev-Kit>.
Note (DESIGN.md 2.4): on JP5/L4T r35 the DTB is flashed, so device-tree
changes need a reflash (or a DTB-partition update), not just a rootfs swap.

## USB gadget — what the host sees

Plug the devkit's USB-C (device-mode) port into a Linux host:

- A **CDC-NCM network interface** appears on the host (usual name
  `enx02cafe550002` — the gadget uses fixed MACs, host side
  `02:ca:fe:55:00:02` — or `usb0` depending on naming policy). NetworkManager
  or any DHCP client gets **192.168.55.100/24** from the device's dnsmasq (no
  gateway/DNS is pushed, so the host keeps its normal uplink).
- The **device** is at **192.168.55.1**; `ping 192.168.55.1` should work
  within a few seconds of boot.
- **RTSP** streams at `rtsp://192.168.55.1:8554/cam0` and `/cam1`
  (e.g. `ffplay rtsp://192.168.55.1:8554/cam0`).
- A **serial console** shows up as `/dev/ttyACM0` on the host with a login
  getty (`picocom /dev/ttyACM0` — baud rate irrelevant for ACM).

Device side: gadget is `usb-gadget.service` (oneshot, waits up to 30 s for
the UDC, restarts on failure), VID:PID `1d6b:0104` (Linux Foundation
composite), functions `ncm.usb0` (+ static 192.168.55.1/24) and `acm.GS0`
(getty on `/dev/ttyGS0`). dnsmasq runs DHCP-only (`port=0`) bound to `usb0`
via `/etc/dnsmasq.d/usb0.conf`.
