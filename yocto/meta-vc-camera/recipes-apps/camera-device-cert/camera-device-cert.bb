SUMMARY = "CA-signed device certificate for camera-streamer"
DESCRIPTION = "Ships a device certificate/key issued by the local CA (see \
scripts/provision-device-cert.sh) into /etc/camera-streamer/tls, together \
with the CA certificate hosts verify against. Installing this suppresses \
camera-streamer-gencert.service, whose unit is conditional on server.crt \
not already existing."
LICENSE = "CLOSED"

# Point this at the --out directory of scripts/provision-device-cert.sh:
#   CAMERA_DEVICE_CERT_DIR = "/path/to/cert"     (local.conf)
# Left unset, the recipe is skipped and the device falls back to generating
# its own unverifiable self-signed cert on first boot.
CAMERA_DEVICE_CERT_DIR ?= ""

python () {
    if not d.getVar('CAMERA_DEVICE_CERT_DIR'):
        raise bb.parse.SkipRecipe(
            'CAMERA_DEVICE_CERT_DIR is not set; run '
            'scripts/provision-device-cert.sh and set it in local.conf')
}

FILESEXTRAPATHS:prepend := "${CAMERA_DEVICE_CERT_DIR}:"
SRC_URI = "file://server.crt file://server.key file://ca.crt"

# The certificate is specific to one provisioning run, so it must not be
# shared through sstate between machines.
PACKAGE_ARCH = "${MACHINE_ARCH}"

S = "${WORKDIR}"

do_install() {
    install -d ${D}${sysconfdir}/camera-streamer/tls
    install -m 0644 ${WORKDIR}/server.crt ${D}${sysconfdir}/camera-streamer/tls/
    install -m 0644 ${WORKDIR}/ca.crt     ${D}${sysconfdir}/camera-streamer/tls/
    # Private key: root-only, and camera-streamer runs as root.
    install -m 0600 ${WORKDIR}/server.key ${D}${sysconfdir}/camera-streamer/tls/
}

FILES:${PN} = "${sysconfdir}/camera-streamer/tls"

# Keep the key out of the -dbg/src packages and out of any stripping logic.
INHIBIT_PACKAGE_STRIP = "1"
INHIBIT_SYSROOT_STRIP = "1"
