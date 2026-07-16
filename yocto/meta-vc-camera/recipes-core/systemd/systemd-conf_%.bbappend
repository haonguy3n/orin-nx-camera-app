# Enable the SoC hardware watchdog via a system.conf drop-in.
FILESEXTRAPATHS:prepend := "${THISDIR}/files:"

SRC_URI += "file://10-watchdog.conf"

do_install:append() {
    install -Dm 0644 ${WORKDIR}/10-watchdog.conf \
        ${D}${sysconfdir}/systemd/system.conf.d/10-watchdog.conf
}

FILES:${PN} += "${sysconfdir}/systemd/system.conf.d"
