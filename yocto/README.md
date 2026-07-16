# yocto/ — meta-vc-camera

Yocto layer for the camera device image (Jetson Orin NX, dual VC MIPI IMX296).
Builds on top of the existing `~/Projects/orin-nx` build (yb/kas, OE-core +
meta-tegra + tegra-demo-distro, branch `scarthgap` / `scarthgap-l4t-r35.x`,
L4T r35.6.4 / JetPack 5, machine `p3768-0000-p3767-0000`, distro `tegrademo`).

## Building

```sh
cd ~/Projects/orin-nx
yb build orin-nx.yml
```

The yml's targets are `demo-image-base` and `swupdate-image-tegra`;
`camera-image` is pulled in through `SWUPDATE_CORE_IMAGE_NAME = "camera-image"`
(the `.swu` update package wraps it), so one build produces both the
tegraflash artifacts for the initial flash and the OTA package.

## How the layer is hooked in

This repo is not cloned into the build project — it is bind-linked in:

- `~/Projects/orin-nx/meta-vc-camera` is a **symlink** to
  `~/Projects/camera-app/yocto/meta-vc-camera` (this working tree), so the
  layer sits at the build-project root like any other layer while its content
  stays version-controlled here. Edits are picked up by the next build with
  no commit/fetch step.
- `orin-nx.yml` has a **url-less repo entry** `camera-app` with layer path
  `meta-vc-camera`. yb (like kas) anchors a url-less repo at the project
  directory and skips git checkout for it, so the layer resolves through the
  symlink and nothing is ever fetched.
- `camera-streamer.bb` builds this tree via **externalsrc** — `EXTERNALSRC`
  is the repo root (derived from the layer's real location, symlink
  resolved) with `OECMAKE_SOURCEPATH` pointing at `embedded/`. The repo
  root matters: externalsrc's git-based checksumming needs `.git` at the
  top, otherwise source edits produce silently stale builds.

Once the repo has a remote, the yml entry can become a normal `url:`/`branch:`
repo with `layers: yocto/meta-vc-camera:` and the symlink can go away.

## What the layer adds

- `recipes-core/usb-gadget/usb-gadget-init.bb` — configfs composite USB
  gadget (systemd oneshot `usb-gadget.service`): CDC-NCM network function +
  ACM serial function, plus dnsmasq config to serve the host its address and
  a getty on the gadget serial port.
- `recipes-apps/camera-streamer/camera-streamer.bb` — the C++ RTSP streaming
  app from `../embedded` (CMake, gstreamer + gst-rtsp-server; runtime deps
  `glib-networking` + `openssl-bin` for the TLS support). Installs
  `camera-streamer.service` (systemd watchdog enabled), the first-boot TLS
  certificate generator `camera-streamer-gencert.service`, and
  `/etc/camera-streamer.conf`.
- `recipes-images/camera-image.bb` — `demo-image-base` + NVIDIA GStreamer
  elements (`gstreamer1.0-plugins-nvarguscamerasrc/-nvvidconv/
  -nvvideo4linux2/-nvvideosinks`), `gstreamer1.0-rtsp-server`, `v4l-utils`,
  `dnsmasq`, `usb-gadget-init`, `camera-streamer`, `vc-isp-tuning`.
- `recipes-bsp/isp-tuning/` — installs `camera_overrides.isp` for libargus
  (currently the measured IMX296C black-level fix; see the README there).
- `recipes-bsp/uefi/` — bbappends fixing the EDK2 BaseTools build on modern
  host GCC (≥15).
- `recipes-kernel/` — VC MIPI driver + device-tree integration (see
  DESIGN.md 2.3/2.4), including the Argus sensor-geometry values and the
  serialized dtb-overlays compile.

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

## OTA updates (swupdate, A/B rootfs)

After the initial flash (redundant layout: `USE_REDUNDANT_FLASH_LAYOUT=1`),
subsequent updates go over the network — kernel, DTB and rootfs land in the
inactive slot and the boot slot flips on success.

### From the host UI (automated)

The camera-viewer application has a "Firmware Update" section in the
control panel sidebar. Click "Select .swu file...", pick the `.swu`
package, then "Upload & Install". The host streams the file to the
device over port 8557, the device installs it via swupdate IPC, and the
progress bar tracks the installation. A reboot activates the new slot:
tick the auto-reboot option before installing, or use the Reboot button
(the control protocol's `reboot` method) after success.

### From the command line (manual)

```sh
cd ~/Projects/orin-nx/build/tmp/deploy/images/p3768-0000-p3767-0000
scp camera-image-p3768-0000-p3767-0000.swu root@192.168.55.1:/tmp/
ssh root@192.168.55.1 'swupdate -i /tmp/camera-image-*.swu && reboot'
```

Each rootfs slot has its own ssh host keys — expect a host-key warning
after every slot switch.

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
