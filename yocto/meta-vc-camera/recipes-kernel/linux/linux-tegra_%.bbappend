# VC MIPI IMX296 camera driver integration for L4T r35.6.x (JetPack 5, kernel 5.10).
#
# Vendored from https://github.com/VC-MIPI-modules/vc_mipi_nvidia
# commit 2a3b1a9693248cf3c9bce7f54f03f49c2f22c476 — see README.md next to
# this file for the full provenance, verification status and the procedure
# to regenerate this patch set on an L4T version bump.
#
# The patches below are VC's official kernel_common_32.3.1+ and
# kernel_Xavier_35.6.0 patch sets with their file paths rewritten for the
# OE4T/linux-tegra-5.10 combined source tree that this recipe builds from
# (kernel/kernel-5.10 -> ${S}, kernel/nvidia -> ${S}/nvidia,
# hardware/nvidia -> ${S}/nvidia), then re-exported with git format-patch
# against the exact SRCREV pinned by meta-tegra for L4T 35.6.4.  All paths
# are relative to ${S}, so no patchdir= parameters are needed.

FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += " \
    file://0001-Added-cropping-position-left-top-to-sensor-image-pro.patch \
    file://0002-Added-implementation-to-set-image-position-and-size-.patch \
    file://0003-Reduced-image-size-limitation-from-width-32-to-4-and.patch \
    file://0004-Added-controls-trigger_mode-io_mode-black_level-sing.patch \
    file://0005-Added-RAW8-grey-RAW10-y10-RAW12-y12-RAW14-y14-rggb8-.patch \
    file://0006-Added-VC-MIPI-Driver-sources-to-Makefile.patch \
    file://0007-Added-VC-MIPI-driver-to-Kconfig.patch \
    file://0008-Changed-Interrupt-Mask-for-csi4-to-emit-CRC-and-mult.patch \
    file://0009-Disable-VB2_BUF_STATE_REQUEUEING-in-vi5_fops.c.patch \
    file://0010-Fixed-compiler-error-for-nv_ar0234-and-nv_hawk_owl.patch \
    file://0011-Handler-function-ready_to_stream-introduced.patch \
    file://0012-Increased-tegra-channel-timeout.patch \
    file://0013-Stability-patch.-work_struct-refactored-to-kthread.patch \
    file://0014-The-function-tegracam_init_ctrl_ranges_by_mode-was-u.patch \
    file://0015-Modified-tegra234-p3768-0000-a0.dts-to-integrate-VC-.patch \
    file://vc-mipi.cfg \
    file://vc_mipi_camera.c \
    file://vc_mipi_core.c \
    file://vc_mipi_core.h \
    file://vc_mipi_modules.c \
    file://vc_mipi_modules.h \
    file://tegra234-camera-vc-mipi-cam.dtsi \
"

# The VC driver sources and the camera .dtsi are files that VC's own flow
# (bin/build.sh, bin/config/configure.sh) copies verbatim into the source
# tree rather than patching.  We mirror that: shipping them as plain files
# keeps VC updates drop-in (copy the new files from src/driver/ and
# src/devicetree/NV_DevKit_OrinNano/) instead of regenerating a 6000-line
# add-files patch.  Only genuine modifications of NVIDIA-owned files are
# carried as patches.
do_patch:append() {
    cp -f ${WORKDIR}/vc_mipi_camera.c \
          ${WORKDIR}/vc_mipi_core.c \
          ${WORKDIR}/vc_mipi_core.h \
          ${WORKDIR}/vc_mipi_modules.c \
          ${WORKDIR}/vc_mipi_modules.h \
          ${S}/nvidia/drivers/media/i2c/
    cp -f ${WORKDIR}/tegra234-camera-vc-mipi-cam.dtsi \
          ${S}/nvidia/platform/t23x/p3768/kernel-dts/cvb/
}

# NVIDIA's custom dtb-overlays make target doesn't declare a dependency on
# scripts/dtc, so whenever dtc needs rebuilding within the same pass (any
# DT re-patch invalidating the kernel workdir), parallel make can exec dtc
# while relinking it -> transient "Permission denied" in
# do_compile_devicetree_overlays. Overlay compilation is cheap; serialize it.
PARALLEL_MAKE:task-compile_devicetree_overlays = ""
