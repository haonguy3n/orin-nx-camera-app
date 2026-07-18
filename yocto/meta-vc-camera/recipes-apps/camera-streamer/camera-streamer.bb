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
#   <repo>/common                (protocol constants shared with host-ui)
# externalsrc (rather than a file:// SRC_URI copy) because yb runs bitbake
# against the live checkout, so builds pick up source edits without a
# commit/fetch step while the app is under active development.
# EXTERNALSRC is the REPO ROOT (not embedded/): the build needs ../common,
# and externalsrc's change detection hashes via git only when the tree has
# .git at its top — pointing at embedded/ silently degraded rebuild
# detection. OECMAKE_SOURCEPATH selects the CMake project inside it.
# realpath first: the layer is reached via a symlink (e.g.
# ~/Projects/orin-nx/meta-vc-camera -> <repo>/yocto/meta-vc-camera), and
# ../.. only exists relative to the layer's real location. If the layer is
# ever moved out of the repo, override EXTERNALSRC in local.conf.
EXTERNALSRC = "${@os.path.normpath(os.path.join(os.path.realpath(d.getVar('VC_CAMERA_LAYERDIR')), '..', '..'))}"
OECMAKE_SOURCEPATH = "${EXTERNALSRC}/embedded"

# json-glib + glib (gio) serve the TCP control protocol (proto/PROTOCOL.md).
# swupdate is needed at runtime for OTA firmware updates (the app talks to
# swupdate's IPC socket at /tmp/sockinstctrl); it's in RDEPENDS, not DEPENDS,
# because the app defines the IPC structs locally (no swupdate headers/libs
# at compile time).
DEPENDS = " \
    glib-2.0 \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
    gstreamer1.0-rtsp-server \
    json-glib \
    openssl \
"

# Pipeline runtime elements: rtph265pay lives in -good (rtp), h265parse in
# -bad (videoparsersbad), and gst-rtsp-server itself builds its serving
# pipeline out of rtpbin (-good rtpmanager) + udpsink (-good udp) + the
# rtsp plugin — without those every mount 503s with "failed to create
# element 'rtpbin'" (found on-target). queue is in coreelements (always
# present). The NVIDIA elements (nvarguscamerasrc, nvv4l2h265enc,
# nvvidconv) are machine-specific and pulled in by camera-image instead.
# -base-app provides appsrc/appsink, which gst-rtsp-server needs for the
# TCP-interleaved transport (the default): without it every TCP client got
# a broken media graph (GST_IS_ELEMENT assertion cascade, no data).
# glib-networking: GIO TLS backend for the control/update servers'
# [server] tls-* support. openssl-bin: first-boot device cert generation
# (camera-streamer-gencert.service). gstreamer1.0: not required by the
# application (secure USB builds its per-camera H.265 pipeline in-process),
# but gst-launch-1.0 is what makes a broken mount diagnosable on target.
RDEPENDS:${PN} += " \
    glib-networking \
    openssl-bin \
    gstreamer1.0 \
    gstreamer1.0-plugins-base-app \
    gstreamer1.0-plugins-good-rtp \
    gstreamer1.0-plugins-good-rtpmanager \
    gstreamer1.0-plugins-good-rtsp \
    gstreamer1.0-plugins-good-udp \
    gstreamer1.0-plugins-good-video4linux2 \
    gstreamer1.0-plugins-bad-videoparsersbad \
    swupdate \
"

# Pin the unit install dir: systemd.pc is not in this recipe's sysroot, so
# CMake's fallback (/usr/lib/systemd/system) would mismatch systemd_system_unitdir.
EXTRA_OECMAKE += "-DSYSTEMD_SYSTEM_UNITDIR=${systemd_system_unitdir}"
EXTRA_OECMAKE += "-DENABLE_SECURE_USB=ON"

SYSTEMD_SERVICE:${PN} = "camera-streamer.service camera-streamer-gencert.service"
SYSTEMD_AUTO_ENABLE = "enable"

CONFFILES:${PN} = "${sysconfdir}/camera-streamer.conf"
