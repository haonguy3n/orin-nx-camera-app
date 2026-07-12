# DIY ISP calibration for the IMX296C (no NVIDIA tuning tools)

VC did not provide a `camera_overrides.isp` for our modules, and NVIDIA's
official tuning suite is partner/NDA-only. This directory is the
do-it-yourself route: the overrides file is **plain text** and its key
sections are hand-editable — publicly documented by working examples such
as RidgeRun's IMX477 calibration (same file format, same ISP):
<https://github.com/RidgeRun/NVIDIA-Jetson-IMX477-RPIV3> and the "Jetson
ISP community tuning" thread on the NVIDIA forums.

What is realistically calibratable by hand, in order of value:

| Section | Keys | How we measure it |
|---|---|---|
| Black level | `opticalBlack.manualBias*` | dark frame (lens capped) |
| Color correction | `colorCorrection.srgbMatrix[0..2]` | ColorChecker capture + least squares (`compute_ccm.py`) |
| AE behavior | `ae.MeanAlg.HigherTarget/LowerTarget`, `ae.MeanAlg.ConvergeSpeed` | taste / scene requirements |
| Framerate floor | `defaults.autoFramerateRange` | keep AE from dropping below the trigger rate |
| Lens shading | `falloff_srfc.controlPoint[r][c]` spline surfaces | flat field; hardest to map — start by just disabling LSC (`ap15Function.lensShading = FALSE`) and check whether your lens even needs it (`raw_stats.py flat` tells you the falloff) |

The runtime knobs (WB mode, saturation, TNR/EE…) do NOT belong in this
file — they're already covered by `set-isp` / `isp-*` config keys.

## Prerequisites

- The camera must work on the **v4l2 path** (`v4l2-ctl` capture) — RAW
  frames bypass the ISP, which is exactly what calibration needs.
- A 24-patch ColorChecker (or any chart with known sRGB values) for the
  CCM; even, daylight-ish illumination.
- Python 3 + numpy on the host (`pip install numpy`).

## Workflow

### 1. Capture RAW on the device

```sh
# black level: lens capped
v4l2-ctl -d /dev/video0 \
    --set-fmt-video=width=1440,height=1080,pixelformat=RG10 \
    --set-ctrl exposure=5000 --set-ctrl gain=0 \
    --stream-mmap --stream-count=8 --stream-skip=4 --stream-to=dark.raw

# flat field: defocused white wall / diffuser, mid exposure (no clipping)
v4l2-ctl ... --stream-to=flat.raw

# color chart: chart filling most of the frame, no clipped patches
v4l2-ctl ... --stream-to=chart.raw
```

(If the driver reports a different RAW10 fourcc, `v4l2-ctl
--list-formats-ext` shows it; the scripts only assume one 16-bit
little-endian word per pixel, which is how Jetson VI writes RAW10.)

### 2. Analyze on the host

```sh
# per-channel black level -> opticalBlack.manualBias* values
./raw_stats.py dark dark.raw --width 1440 --height 1080

# corner falloff per channel -> is lens shading correction even needed?
./raw_stats.py flat flat.raw --width 1440 --height 1080

# gray-world WB gains from any neutral scene
./raw_stats.py gray flat.raw --width 1440 --height 1080

# CCM from the chart capture: give the pixel coordinates of the centers of
# the four CORNER PATCHES (dark-skin, bluish-green, white, black on a
# standard 24-patch chart), row-major chart orientation
./compute_ccm.py chart.raw --width 1440 --height 1080 --black 64 \
    --corners 180,140 1260,150 1250,940 190,930
```

`compute_ccm.py` prints ready-to-paste `colorCorrection.srgbMatrix[...]`
lines (and the measured WB gains for reference).

### 3. Edit, deploy, iterate

1. Start from `template_camera_overrides.isp`, paste your measured values.
2. Copy it to `yocto/meta-vc-camera/recipes-bsp/isp-tuning/files/camera_overrides.isp`
   and uncomment `vc-isp-tuning` in `camera-image.bb` — or, for fast
   iteration on a running device:

   ```sh
   scp camera_overrides.isp root@192.168.55.1:/var/nvidia/nvcam/settings/
   ssh root@192.168.55.1 'chmod 664 /var/nvidia/nvcam/settings/camera_overrides.isp \
       && systemctl restart camera-streamer'
   ```

3. Look at the argus stream, compare against the chart, adjust, repeat.
   Argus **silently ignores** a broken file — if a deliberately absurd
   change (e.g. saturation-killing CCM) has no visible effect, the file
   isn't being loaded (path/permissions).

## Limits of the DIY route

Demosaic quality, noise-profile tables and the finer AWB tuning stay at
NVIDIA's defaults — those genuinely need the partner tools. In practice
black level + CCM + AE targets fix the visible 90% (wrong colors, tint,
brightness hunting). If color fidelity requirements outgrow this, the
escalation path is an NVIDIA camera tuning partner (RidgeRun, Leopard,
D3 …), whose deliverable is again a `camera_overrides.isp` that deploys
through the same recipe.
