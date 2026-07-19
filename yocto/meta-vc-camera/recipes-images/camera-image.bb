SUMMARY = "Camera device image: demo-image-base plus the VC MIPI streaming stack"

# Cross-layer require: meta-tegrademo prepends itself to BBPATH in its
# layer.conf (BBPATH =. "${LAYERDIR}:"), so this path resolves to
# .../tegra-demo-distro/layers/meta-tegrademo/recipes-demo/images/demo-image-base.bb
# and its relative `require demo-image-common.inc` resolves next to it.
require recipes-demo/images/demo-image-base.bb

# Secure USB is integrated into camera-streamer; never pull the former
# standalone endpoint package into the image.
IMAGE_INSTALL:remove = "secure-usb secure-usb-device"
PACKAGE_EXCLUDE:append = " secure-usb secure-usb-device"

remove_legacy_secure_usb_unit() {
    rm -f ${IMAGE_ROOTFS}${systemd_system_unitdir}/secure-usb-device.service
    rm -f ${IMAGE_ROOTFS}${systemd_system_unitdir}/multi-user.target.wants/secure-usb-device.service
}
ROOTFS_POSTPROCESS_COMMAND += "remove_legacy_secure_usb_unit;"

DESCRIPTION = "demo-image-base plus: NVIDIA GStreamer elements (Argus source, \
VIC conversion, V4L2 NVENC/NVDEC, sinks), gst-rtsp-server, v4l-utils, the USB \
gadget (CDC-NCM + ACM) and the camera-streamer application."

# NVIDIA element package names verified in
# meta-tegra/recipes-multimedia/gstreamer/ on scarthgap-l4t-r35.x (r35.6.4).
IMAGE_INSTALL += " \
    gstreamer1.0-plugins-nvarguscamerasrc \
    gstreamer1.0-plugins-nvvidconv \
    gstreamer1.0-plugins-nvvideo4linux2 \
    gstreamer1.0-plugins-nvvideosinks \
    gstreamer1.0-rtsp-server \
    v4l-utils \
    dnsmasq \
    usb-gadget-init \
    camera-streamer \
"

# CA-signed device certificate, when one has been provisioned (see
# scripts/provision-device-cert.sh). Unset, the image keeps the first-boot
# self-signed cert from camera-streamer-gencert.service, which hosts can only
# pin, not verify.
IMAGE_INSTALL += "${@'camera-device-cert' if d.getVar('CAMERA_DEVICE_CERT_DIR') else ''}"

# YuNet face-detection model, always shipped so on-device face detection runs
# out of the box (the app auto-enables it when the model file is present -- see
# camera-streamer [detect]). The model recipe fetches the ONNX at build time.
IMAGE_INSTALL += "camera-face-model"

# Static ISP tuning for the color IMX296C: currently the DIY black-level
# override (sensor pedestal subtraction — fixes the pink haze), measured
# and verified on target 2026-07-12. Extend the file via tools/isp-tuning
# (WB/CCM calibration with a ColorChecker) as tuning progresses.
IMAGE_INSTALL += "vc-isp-tuning"
