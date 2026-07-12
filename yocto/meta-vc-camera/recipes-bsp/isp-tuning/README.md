# ISP tuning for the color IMX296C (argus path)

Jetson ISP tuning has three layers; this recipe covers the middle one.

1. **Runtime controls** — white balance, saturation, TNR/EE, AE
   antibanding, exposure compensation. Already supported: `set-isp` in the
   control protocol (`proto/PROTOCOL.md`), the ISP group in the host UI, and
   `isp-*` keys in `/etc/camera-streamer.conf`. Nothing to do here.
2. **Static tuning file** (`camera_overrides.isp`) — lens shading, color
   correction matrix, gamma, demosaic parameters. libargus loads it from
   `/var/nvidia/nvcam/settings/` at camera open and applies it over the
   default tuning. **This recipe installs that file.** Sources for it, in
   order of preference: Vision Components support, an NVIDIA tuning
   partner, or **do it yourself** — the file is plain text, and
   `tools/isp-tuning/` (repo root) has the full DIY workflow: RAW captures
   via the v4l2 path, scripts for black level / falloff / white balance /
   CCM, and a commented template to start from.
3. **Full sensor characterization** — NVIDIA's ISP tuning suite (partner
   tooling), lab targets, real hardware. Out of scope for this layer;
   its output is again a `camera_overrides.isp`, deployed the same way.

## Enabling

1. Put the tuning file at `files/camera_overrides.isp` (next to this
   README).
2. Add the package to the image — in `recipes-images/camera-image.bb`,
   uncomment:

   ```bitbake
   IMAGE_INSTALL += "vc-isp-tuning"
   ```

Without the file present, building `vc-isp-tuning` fails at do_fetch —
that's why it is not in the image by default.

## Verifying on target

```sh
ls -l /var/nvidia/nvcam/settings/camera_overrides.isp   # must be mode 664
systemctl restart camera-streamer                        # reopen the sensor
```

A wrong mode or a syntactically broken file is *silently ignored* by
Argus — compare image saturation/shading before and after to confirm it
took effect.
