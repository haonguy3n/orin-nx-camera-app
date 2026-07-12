SUMMARY = "Camera device image: demo-image-base plus the VC MIPI streaming stack"

# Cross-layer require: meta-tegrademo prepends itself to BBPATH in its
# layer.conf (BBPATH =. "${LAYERDIR}:"), so this path resolves to
# .../tegra-demo-distro/layers/meta-tegrademo/recipes-demo/images/demo-image-base.bb
# and its relative `require demo-image-common.inc` resolves next to it.
require recipes-demo/images/demo-image-base.bb

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

# Static ISP tuning for the color IMX296C. Uncomment once VC's
# camera_overrides.isp is dropped into
# recipes-bsp/isp-tuning/files/ (see the README there).
#IMAGE_INSTALL += "vc-isp-tuning"
