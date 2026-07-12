SUMMARY = "Argus ISP tuning overrides for the VC MIPI IMX296C"
DESCRIPTION = "Installs camera_overrides.isp, which libargus applies on top \
of the default ISP tuning for every sensor (lens shading, CCM, gamma, \
demosaic parameters). The file itself is not in this layer: obtain it from \
Vision Components for the IMX296C and drop it into files/ — see README.md \
next to this recipe. Only relevant for the argus capture path (color \
modules); the pure-V4L2 path bypasses the ISP."
LICENSE = "CLOSED"

inherit allarch

SRC_URI = "file://camera_overrides.isp"

# libargus reads override tuning from this fixed path at camera open.
ISP_SETTINGS_DIR = "/var/nvidia/nvcam/settings"

do_install() {
    install -d ${D}${ISP_SETTINGS_DIR}
    # 0664 is what nvidia's own tooling expects; wrong permissions make
    # Argus silently ignore the file.
    install -m 0664 ${WORKDIR}/camera_overrides.isp ${D}${ISP_SETTINGS_DIR}/
}

FILES:${PN} = "${ISP_SETTINGS_DIR}"
