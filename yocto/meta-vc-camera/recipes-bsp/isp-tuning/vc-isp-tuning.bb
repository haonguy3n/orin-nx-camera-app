SUMMARY = "Argus ISP tuning overrides for the VC MIPI IMX296C"
DESCRIPTION = "Installs camera_overrides.isp, which libargus applies on top \
of the default ISP tuning for every sensor (lens shading, CCM, gamma, \
demosaic parameters). The file itself is not in this layer: obtain it from \
Vision Components for the IMX296C and drop it into files/ — see README.md \
next to this recipe. Only relevant for the argus capture path (color \
modules); the pure-V4L2 path bypasses the ISP."
LICENSE = "CLOSED"

inherit allarch

# The tuning file is deliberately not in the layer (get it from VC or
# calibrate it with tools/isp-tuning/ in the source repo). A plain
# file:// SRC_URI would break PARSING of the whole layer while the file
# is absent (bitbake checksums file:// entries at parse time), so
# reference it only once it exists; until then, building this recipe
# fails in do_install with a pointer to the README.
VC_ISP_DIR := "${THISDIR}/files"
SRC_URI = "${@'file://camera_overrides.isp' if os.path.exists(d.getVar('VC_ISP_DIR') + '/camera_overrides.isp') else ''}"

# libargus reads override tuning from this fixed path at camera open.
ISP_SETTINGS_DIR = "/var/nvidia/nvcam/settings"

do_install() {
    if [ ! -e "${WORKDIR}/camera_overrides.isp" ]; then
        bbfatal "camera_overrides.isp is missing: drop the tuning file into ${VC_ISP_DIR}/ (see the README next to this recipe) before adding vc-isp-tuning to the image"
    fi
    install -d ${D}${ISP_SETTINGS_DIR}
    # 0664 is what nvidia's own tooling expects; wrong permissions make
    # Argus silently ignore the file.
    install -m 0664 ${WORKDIR}/camera_overrides.isp ${D}${ISP_SETTINGS_DIR}/
}

FILES:${PN} = "${ISP_SETTINGS_DIR}"
