SUMMARY = "Dual VC MIPI IMX296 camera streamer (GStreamer RTSP server)"
DESCRIPTION = "C++ application supervising the capture -> NVENC -> RTSP \
pipelines for the two IMX296 sensors. Installs the camera-streamer binary, \
/etc/camera-streamer.conf and camera-streamer.service from its own CMake tree."
HOMEPAGE = "https://github.com/haonguy3n/camera-app"
# Private project code; switch to a real license + LIC_FILES_CHKSUM once the
# source tree carries a LICENSE file.
LICENSE = "CLOSED"

PV = "0.1"

inherit cmake pkgconfig systemd externalsrc

# The application sources live in the same git repository as this layer:
#   <repo>/yocto/meta-vc-camera  (this layer, VC_CAMERA_LAYERDIR)
#   <repo>/embedded              (the CMake project)
# externalsrc (rather than a file:// SRC_URI copy) because yb runs bitbake
# against the live checkout — repos/camera-app in the build project is (a
# symlink to) this working tree, so building straight from it picks up source
# edits without requiring commits or re-fetching, which is what we want while
# the app is under active development. The path is derived from the layer
# location, so it works wherever the repo is checked out or mounted.
EXTERNALSRC = "${@os.path.normpath(d.getVar('VC_CAMERA_LAYERDIR') + '/../../embedded')}"

DEPENDS = " \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-rtsp-server \
"

# Pipeline runtime elements: rtph265pay lives in -good (rtp), h265parse in
# -bad (videoparsersbad). The NVIDIA elements (nvarguscamerasrc, nvv4l2h265enc,
# nvvidconv) are machine-specific and pulled in by camera-image instead.
RDEPENDS:${PN} += " \
    gstreamer1.0-plugins-good-rtp \
    gstreamer1.0-plugins-bad-videoparsersbad \
"

# Pin the unit install dir: systemd.pc is not in this recipe's sysroot, so
# CMake's fallback (/usr/lib/systemd/system) would mismatch systemd_system_unitdir.
EXTRA_OECMAKE += "-DSYSTEMD_SYSTEM_UNITDIR=${systemd_system_unitdir}"

SYSTEMD_SERVICE:${PN} = "camera-streamer.service"
SYSTEMD_AUTO_ENABLE = "enable"

CONFFILES:${PN} = "${sysconfdir}/camera-streamer.conf"
