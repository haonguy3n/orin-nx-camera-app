#!/bin/sh
# usb-gadget — bring up / tear down the ConfigFS USB composite gadget:
#   * CDC-NCM network function -> usb0, 192.168.55.1/24 on the device side
#     (the host side gets 192.168.55.100 via dnsmasq, see /etc/dnsmasq.d/usb0.conf)
#   * ACM serial function      -> /dev/ttyGS0, with a getty on it
#
# Modelled on stock L4T nv-l4t-usb-device-mode, but minimal and self-contained.
set -eu

GADGET_DIR=/sys/kernel/config/usb_gadget/vc-camera
DEV_IP=192.168.55.1/24

# Fixed, locally administered MACs so both ends get stable interface identity.
DEV_MAC="02:ca:fe:55:00:01"
HOST_MAC="02:ca:fe:55:00:02"

start() {
    # configfs / function drivers are built into the L4T kernel in the stock
    # tegra_defconfig; the modprobes are harmless no-ops in that case.
    [ -d /sys/kernel/config ] || modprobe configfs 2>/dev/null || true
    mountpoint -q /sys/kernel/config || \
        mount -t configfs none /sys/kernel/config
    modprobe libcomposite 2>/dev/null || true
    modprobe usb_f_ncm 2>/dev/null || true
    modprobe usb_f_acm 2>/dev/null || true

    if [ -s "$GADGET_DIR/UDC" ]; then
        echo "usb-gadget: already bound to $(cat "$GADGET_DIR/UDC")"
        return 0
    fi

    mkdir -p "$GADGET_DIR"
    cd "$GADGET_DIR"

    echo 0x1d6b > idVendor      # Linux Foundation
    echo 0x0104 > idProduct     # Multifunction Composite Gadget
    echo 0x0200 > bcdUSB
    echo 0x0100 > bcdDevice
    # IAD device class so composite enumeration works everywhere
    echo 0xEF > bDeviceClass
    echo 0x02 > bDeviceSubClass
    echo 0x01 > bDeviceProtocol

    serial=$(tr -d '\0' < /proc/device-tree/serial-number 2>/dev/null || true)
    mkdir -p strings/0x409
    echo "${serial:-0000000000}"     > strings/0x409/serialnumber
    echo "VC Camera"                 > strings/0x409/manufacturer
    echo "VC MIPI dual IMX296 camera" > strings/0x409/product

    mkdir -p configs/c.1/strings/0x409
    echo "CDC-NCM network + ACM serial" > configs/c.1/strings/0x409/configuration
    echo 250 > configs/c.1/MaxPower

    # --- CDC-NCM network function ---
    if [ ! -d functions/ncm.usb0 ]; then
        mkdir functions/ncm.usb0
        echo "$DEV_MAC"  > functions/ncm.usb0/dev_addr
        echo "$HOST_MAC" > functions/ncm.usb0/host_addr
    fi
    [ -e configs/c.1/ncm.usb0 ] || ln -s functions/ncm.usb0 configs/c.1/

    # --- ACM serial function (/dev/ttyGS0) ---
    [ -d functions/acm.GS0 ] || mkdir functions/acm.GS0
    [ -e configs/c.1/acm.GS0 ] || ln -s functions/acm.GS0 configs/c.1/

    # --- bind to the UDC; it may probe later during boot, so wait a little.
    # The systemd unit also has Restart=on-failure as a second safety net.
    udc=
    i=0
    while [ $i -lt 30 ]; do
        udc=$(ls /sys/class/udc 2>/dev/null | head -n 1)
        [ -n "$udc" ] && break
        i=$((i + 1))
        sleep 1
    done
    if [ -z "$udc" ]; then
        echo "usb-gadget: no UDC appeared in /sys/class/udc" >&2
        exit 1
    fi
    echo "$udc" > UDC

    # --- force the OTG port into device role. On the p3768 devkit the
    # USB-C port's role switch comes up as "none", so the bound gadget
    # never attaches to the host (usb0 stays NO-CARRIER, nothing enumerates
    # on the PC). Stock L4T's nv-l4t-usb-device-mode forces this too.
    for role in /sys/class/usb_role/*/role; do
        [ -e "$role" ] || continue
        echo device > "$role" 2>/dev/null || true
    done

    # --- device-side network config; the netdev exists once ncm.usb0 does.
    iface=$(cat functions/ncm.usb0/ifname)
    ip link set "$iface" up
    ip addr replace "$DEV_IP" dev "$iface"
    echo "usb-gadget: bound to $udc, $iface at $DEV_IP"
}

stop() {
    [ -d "$GADGET_DIR" ] || return 0
    cd "$GADGET_DIR"
    # unbind first, then dismantle in reverse creation order
    echo "" > UDC 2>/dev/null || true
    rm -f configs/c.1/ncm.usb0 configs/c.1/acm.GS0
    rmdir configs/c.1/strings/0x409 configs/c.1 \
          functions/ncm.usb0 functions/acm.GS0 \
          strings/0x409 2>/dev/null || true
    cd /
    rmdir "$GADGET_DIR" 2>/dev/null || true
}

case "${1:-}" in
    start) start ;;
    stop)  stop ;;
    *) echo "usage: $0 start|stop" >&2; exit 2 ;;
esac
