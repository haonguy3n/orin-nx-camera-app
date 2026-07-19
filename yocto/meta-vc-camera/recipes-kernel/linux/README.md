# VC MIPI IMX296 kernel integration (linux-tegra 5.10, L4T r35.6.x)

Kernel-side integration of the Vision Components MIPI CSI-2 driver for
**dual VC MIPI IMX296** (1-lane each, RAW10, 1440x1080) on the Jetson Orin NX
devkit (machine `p3768-0000-p3767-0000`, meta-tegra branch
`scarthgap-l4t-r35.x`, L4T **35.6.4**, kernel 5.10 / JetPack 5).

## Provenance

| What | Value |
|---|---|
| VC repo | <https://github.com/VC-MIPI-modules/vc_mipi_nvidia> |
| VC commit vendored | `2a3b1a9693248cf3c9bce7f54f03f49c2f22c476` (2026-06-08, "DEV-267 Support of older L4T versions discontinued.") |
| VC patch sets used | `patch/kernel_common_32.3.1+` + `patch/kernel_Xavier_35.6.0` (the set VC's `bin/config/L4T/R35.6.0.sh` applies for Orin NX / Orin Nano SOMs) |
| VC driver sources | `src/driver/vc_mipi_{camera,core,modules}.{c,h}` — verbatim copies |
| VC device tree | `src/devicetree/NV_DevKit_OrinNano/tegra234-camera-vc-mipi-cam.dtsi` — copied with two config `#define` changes (see below) |
| Kernel tree patched against | `github.com/OE4T/linux-tegra-5.10`, branch `oe4t-patches-l4t-r35.6.4`, SRCREV `050b88cc9d0ffc4ace5f54ea0bc3f2b46f163486` (the exact pin in meta-tegra `recipes-kernel/linux/linux-tegra_5.10.bb`) |

## How this maps VC's script flow to Yocto

VC's `setup.sh` operates on the L4T `public_sources.tbz2` layout
(`kernel/kernel-5.10`, `kernel/nvidia`, `hardware/nvidia`). meta-tegra's
`linux-tegra_5.10.bb` instead builds the **OE4T combined tree**, where:

| L4T public_sources path | OE4T tree path (`${S}`) |
|---|---|
| `kernel/kernel-5.10/` | `${S}/` |
| `kernel/nvidia/` | `${S}/nvidia/` |
| `hardware/nvidia/` (platform/, soc/) | `${S}/nvidia/` |

All VC patches were path-rewritten accordingly, applied to the pinned SRCREV
with `git am --whitespace=fix --ignore-whitespace` (same options VC's
`setup.sh` uses), and re-exported with `git format-patch` — so the shipped
`files/00*.patch` apply **exactly** (no offsets/fuzz) against the 35.6.4
sources this recipe builds. All paths are relative to `${S}`; no `patchdir=`
parameters are needed. Note the recipe uses kernel-yocto's `do_patch`
(kgit-s2q, i.e. git-am based), which is exactly how the series was verified.

Pieces that VC's flow copies as whole files (`bin/build.sh` copies
`src/driver/*`; `configure.sh` copies the camera `.dtsi`) are shipped as
plain `file://` entries and copied in `do_patch:append()` instead of being
converted into a giant add-files patch — on a VC driver update they are
drop-in replaceable and trivially diffable against VC's `src/` directory.
Only modifications of NVIDIA-owned files travel as patches.

### Patch map (shipped file ← VC original)

| Shipped | VC original (relative to `patch/`) |
|---|---|
| 0001–0003 | `kernel_common_32.3.1+`: cropping position, image position/size via v4l2, min image size 32→4 |
| 0004 | `kernel_Xavier_35.6.0/0001-Added-controls-trigger_mode-io_mode-black_level-sing.patch` |
| 0005 | `...0001-Added-RAW8-grey-RAW10-y10-RAW12-y12-RAW14-y14-rggb8-.patch` (GREY/Y10/Y12… format support in VI) |
| 0006 | `...0001-Added-VC-MIPI-Driver-sources-to-Makefile.patch` — **defconfig hunks dropped** (handled by `vc-mipi.cfg`; they touched `defconfig`/`tegra_android_defconfig`, not the `tegra_defconfig` this recipe uses) |
| 0007 | `...0001-Added-VC-MIPI-driver-to-Kconfig.patch` |
| 0008 | `...0001-Changed-Interrupt-Mask-for-csi4-...patch` |
| 0009 | `...0001-Disable-VB2_BUF_STATE_REQUEUEING-in-vi5_fops.c.patch` |
| 0010 | `...0001-Fixed-compiler-error-for-nv_ar0234-and-nv_hawk_owl.patch` (needed after 0004's header changes) |
| 0011 | `...0001-Handler-function-ready_to_stream-introduced.patch` |
| 0012 | `...0001-Increased-tegra-channel-timeout.patch` |
| 0013 | `...0001-Stability-patch.-work_struct-refactored-to-kthread.patch` |
| 0014 | `...0001-The-function-tegracam_init_ctrl_ranges_by_mode-was-u.patch` |
| 0015 | `...0001-Modified-tegra234-p3768-0000-a0.dts-to-integ.patch` — includes `tegra234-camera-vc-mipi-cam.dtsi` into the p3768 carrier dtsi and disables the stock IMX219/IMX477 devkit camera DT |

**Deliberately dropped from VC's set:** `0001-Added-.gitignore.patch`
(irrelevant), and the DT patches for other boards
(`tegra194-p2888...` AGX Xavier, `tegra194-p3509-0000-a00` Xavier NX,
`tegra234-p3509-a02` Orin module on the p3509 carrier). Our
`KERNEL_DEVICETREE` is `tegra234-p3767-0000-super-p3768-0000-a0.dtb`, whose
include chain is `...super-p3768-0000-a0.dts → tegra234-p3767-0000-p3768-0000-a0.dts
→ cvb/tegra234-p3768-0000-a0.dtsi` — the file patch 0015 modifies.

### Device tree configuration (dual IMX296)

`files/tegra234-camera-vc-mipi-cam.dtsi` is VC's NV_DevKit_OrinNano
reference file with only the configuration `#define`s changed, exactly as VC
instructs in the file header:

- `VC_MIPI_LANES 1` (IMX296 is 1-lane only; upstream default is 2)
- `VC_MIPI_METADATA_H "2"` (IMX296 per VC's table; upstream default "1")
- both cameras enabled (upstream default): CAM1 = i2c-mux bus 0, addr 0x1a,
  `serial_b` (CSI port 1) → `devnode video0`; CAM0 = i2c-mux bus 1, addr
  0x1a, `serial_c` (CSI port 2) → `devnode video1`. I2C addresses, GPIOs
  (`CAM0_PWDN` H6, `CAM1_PWDN` AC0, i2c-mux GPIO CC3), port-index and
  tegra-camera-platform entries are VC's values, unmodified.

## 35.6.0 vs 35.6.4 delta assessment

VC officially lists L4T 35.6.0 for Orin NX. Applying their 35.6.0 set to the
real 35.6.4 tree (OE4T SRCREV above, which also carries OE4T's own patches):

- `git am -3 --whitespace=fix --ignore-whitespace`: **all 15 patches apply
  cleanly**.
- plain GNU `patch -p1` (quilt-style): all apply; only the three
  `kernel_common` patches land at small line offsets, no fuzz failures.

Conclusion: NVIDIA did not touch the affected camera/VI/CSI/RTCPU files
incompatibly between 35.6.0 and 35.6.4. The shipped patches are re-exported
against 35.6.4, so they are exact for this pin.

## Verification performed (and not performed)

Verified:
1. **Patch application** against the exact pinned SRCREV
   `050b88cc9d0ffc4ace5f54ea0bc3f2b46f163486`, both with `git am` (what
   kernel-yocto's kgit-s2q effectively does) and plain GNU `patch`.
2. **Device tree compiles**: with patches + dtsi + driver files in place,
   `make ARCH=arm64 tegra_defconfig && make ARCH=arm64 dtbs` built the full
   DTB set including `tegra234-p3767-0000-super-p3768-0000-a0.dtb`;
   decompiling it shows both `vc_mipi@1a` sensor nodes (`num_lanes = "1"`,
   `devnode` video0/video1, `serial_b`/`serial_c`), the NVCSI/VI channel
   bindings and both `tegra-camera-platform` module entries.

NOT verified:
3. **C compilation of the driver and patched framework** — no aarch64
   cross-toolchain on the host (clang 22 cannot build a 5.10 kernel). The
   patched framework files applied cleanly and the driver sources are
   exactly what VC builds on 35.6.x, so risk is low, but the first
   `bitbake virtual/kernel` is the real compile check.

## Regenerating on an L4T / VC bump

1. Read the new meta-tegra pin: `SRCREV`/`SRCBRANCH` in
   `recipes-kernel/linux/linux-tegra_5.10.bb` (or the 36.x equivalent — note
   JP6/L4T 36.x uses a completely different mechanism: nvidia-oot + .dtbo
   overlays; this whole directory does not carry over).
2. Clone/fetch that exact SRCREV of `github.com/OE4T/linux-tegra-5.10`.
3. In a checkout of `vc_mipi_nvidia`, take the patch dirs referenced by
   `PATCHES=(...)` in `bin/config/L4T/R<ver>.sh` for the Orin NX SOM.
4. Rewrite paths: `sed -e 's|kernel/kernel-5.10/||g' -e
   's|kernel/nvidia/|nvidia/|g' -e 's|hardware/nvidia/|nvidia/|g'`; drop the
   `.gitignore` patch, other-board DT patches, and the defconfig hunks of
   the Makefile patch.
5. `git am --whitespace=fix --ignore-whitespace` the series onto the SRCREV;
   resolve any conflicts; `git format-patch` into `files/`.
6. Copy the new `src/driver/vc_mipi_*` over the files here; copy the new
   `src/devicetree/NV_DevKit_OrinNano/tegra234-camera-vc-mipi-cam.dtsi` and
   re-apply the two IMX296 `#define` changes (lanes=1, metadata="2").
7. Update the provenance table above (VC commit ⇄ L4T version pair — bump
   only together).

## Open risks / integrator checklist before building

- **Compile check outstanding**: run `bitbake virtual/kernel` first; the
  driver builds into the kernel image (`CONFIG_NV_VIDEO_VC_MIPI=y`, see
  `vc-mipi.cfg` for why it cannot be `=m`).
- **Mono vs color IMX296 — resolved** (docs/DESIGN.md §7): cam0 is the
  color IMX296C, cam1 the mono IMX296 (currently unplugged/disabled). The
  dtsi still uses VC's defaults `mode_type "bayer"` / `pixel_phase "rggb"`
  for both, which is why the mono sensor only exposes RG10 and pure-V4L2
  grey capture needs a DT mode change (gray/Y10) — the M3 sync-capture
  prerequisite.
- Patch 0015 **disables the stock IMX219/IMX477 devkit camera DT** — Rasp-Pi
  cams on the devkit stop working (expected for this product).
- The patches apply to the shared `linux-tegra` sources for **every** tegra
  machine built from this workspace, not just p3768 (framework changes are
  VC's official set and DT changes are p3768-only, but be aware if other
  Jetson machines are built from the same layers).
- DTB is flashed on JP5: after building, reflash (at least the DTB
  partition) — a plain rootfs update won't update the DT.
- Acceptance check after flash: `/dev/video0` + `/dev/video1`;
  `v4l2-ctl -d /dev/video0 --list-formats-ext` shows 1440x1080 RAW10;
  `v4l2-ctl -d /dev/video0 --list-ctrls` shows `trigger_mode`, `io_mode`,
  `black_level`, `single_trigger`.
- Physical mapping for bring-up: devkit CAM1 connector (i2c-mux bus 0,
  `serial_b`) → `video0`; CAM0 connector (i2c-mux bus 1, `serial_c`) →
  `video1`. If only one camera is fitted, set the corresponding
  `VC_MIPI_CAM_x` define to 0 in the dtsi — a missing sensor otherwise
  leaves a dead tegra-camera-platform entry (harmless for V4L2, confusing
  for Argus).
