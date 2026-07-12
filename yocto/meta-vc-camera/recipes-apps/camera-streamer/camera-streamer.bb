SUMMARY = "Dual VC MIPI IMX296 camera streamer (GStreamer RTSP server)"
DESCRIPTION = "C++ application supervising the capture -> NVENC -> RTSP \
pipelines for the two IMX296 sensors. Installs the camera-streamer binary, \
/etc/camera-streamer.conf and camera-streamer.service from its own CMake tree."
HOMEPAGE = "https://github.com/haonguy3n/camera-app"
# Private project code; switch to a real license + LIC_FILES_CHKSUM once the
# source tree carries a LICENSE file.
LICENSE = "CLOSED"

PV = "0.4"

inherit cmake pkgconfig systemd externalsrc

# The application sources live in the same git repository as this layer:
#   <repo>/yocto/meta-vc-camera  (this layer, VC_CAMERA_LAYERDIR)
#   <repo>/embedded              (the CMake project)
# externalsrc (rather than a file:// SRC_URI copy) because yb runs bitbake
# against the live checkout, so builds pick up source edits without a
# commit/fetch step while the app is under active development.
# realpath first: the layer is reached via a symlink (e.g.
# ~/Projects/orin-nx/meta-vc-camera -> <repo>/yocto/meta-vc-camera), and
# ../../embedded only exists relative to the layer's real location. If the
# layer is ever moved out of the repo, override EXTERNALSRC in local.conf.
EXTERNALSRC = "${@os.path.normpath(os.path.join(os.path.realpath(d.getVar('VC_CAMERA_LAYERDIR')), '..', '..', 'embedded'))}"

# json-glib + glib (gio) serve the TCP control protocol (proto/PROTOCOL.md).
DEPENDS = " \
    glib-2.0 \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-rtsp-server \
    json-glib \
"

# Pipeline runtime elements: rtph265pay lives in -good (rtp), h265parse in
# -bad (videoparsersbad), and gst-rtsp-server itself builds its serving
# pipeline out of rtpbin (-good rtpmanager) + udpsink (-good udp) + the
# rtsp plugin — without those every mount 503s with "failed to create
# element 'rtpbin'" (found on-target). queue is in coreelements (always
# present). The NVIDIA elements (nvarguscamerasrc, nvv4l2h265enc,
# nvvidconv) are machine-specific and pulled in by camera-image instead.
RDEPENDS:${PN} += " \
    gstreamer1.0-plugins-good-rtp \
    gstreamer1.0-plugins-good-rtpmanager \
    gstreamer1.0-plugins-good-rtsp \
    gstreamer1.0-plugins-good-udp \
    gstreamer1.0-plugins-bad-videoparsersbad \
"

# Pin the unit install dir: systemd.pc is not in this recipe's sysroot, so
# CMake's fallback (/usr/lib/systemd/system) would mismatch systemd_system_unitdir.
EXTRA_OECMAKE += "-DSYSTEMD_SYSTEM_UNITDIR=${systemd_system_unitdir}"

SYSTEMD_SERVICE:${PN} = "camera-streamer.service"
SYSTEMD_AUTO_ENABLE = "enable"

CONFFILES:${PN} = "${sysconfdir}/camera-streamer.conf"
