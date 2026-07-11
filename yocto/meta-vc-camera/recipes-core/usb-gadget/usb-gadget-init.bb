SUMMARY = "ConfigFS USB composite gadget (CDC-NCM + ACM) for the camera device"
DESCRIPTION = "Systemd oneshot that builds a composite USB gadget in \
/sys/kernel/config/usb_gadget/: a CDC-NCM network function (usb0, static \
192.168.55.1/24 on the device, DHCP for the host via dnsmasq) and an ACM \
serial function with a getty on /dev/ttyGS0."
LICENSE = "MIT"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/MIT;md5=0835ade698e0bcf8506ecda2f7b4f302"

inherit systemd features_check

# Not "usbgadget": that is a machine feature, and meta-tegra's tegra-common.inc
# doesn't set it anyway (the L4T kernel has configfs gadget support regardless).
REQUIRED_DISTRO_FEATURES = "systemd"

SRC_URI = " \
    file://usb-gadget.sh \
    file://usb-gadget.service \
    file://usb0-dnsmasq.conf \
    file://dnsmasq-usb-gadget-override.conf \
"

S = "${WORKDIR}"

do_install() {
    install -d ${D}${sbindir}
    install -m 0755 ${WORKDIR}/usb-gadget.sh ${D}${sbindir}/usb-gadget

    install -d ${D}${systemd_system_unitdir}
    install -m 0644 ${WORKDIR}/usb-gadget.service ${D}${systemd_system_unitdir}/

    # Getty on the gadget serial port (same enable-by-symlink pattern as
    # oe-core's systemd-serialgetty).
    install -d ${D}${sysconfdir}/systemd/system/getty.target.wants
    ln -sf ${systemd_system_unitdir}/serial-getty@.service \
        ${D}${sysconfdir}/systemd/system/getty.target.wants/serial-getty@ttyGS0.service

    # DHCP config for the host side; the stock dnsmasq.service already reads
    # conf-dir /etc/dnsmasq.d (ExecStart passes -7 /etc/dnsmasq.d).
    install -d ${D}${sysconfdir}/dnsmasq.d
    install -m 0644 ${WORKDIR}/usb0-dnsmasq.conf ${D}${sysconfdir}/dnsmasq.d/usb0.conf

    # Order dnsmasq after the gadget so usb0 exists when it starts.
    install -d ${D}${sysconfdir}/systemd/system/dnsmasq.service.d
    install -m 0644 ${WORKDIR}/dnsmasq-usb-gadget-override.conf \
        ${D}${sysconfdir}/systemd/system/dnsmasq.service.d/usb-gadget.conf
}

SYSTEMD_SERVICE:${PN} = "usb-gadget.service"
SYSTEMD_AUTO_ENABLE = "enable"

# The gadget script uses ip/mountpoint (busybox variants would do, but be
# explicit); dnsmasq serves the host its address.
RDEPENDS:${PN} = "dnsmasq iproute2 util-linux-mountpoint"

FILES:${PN} += "${systemd_system_unitdir}"
CONFFILES:${PN} = "${sysconfdir}/dnsmasq.d/usb0.conf"
