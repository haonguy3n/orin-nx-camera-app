// Resolution of the [server] listen= setting, shared by the RTSP and
// control servers so both bind the same network.
#pragma once

#include <string>

// First IPv4 address of a network interface, or "" if the interface is
// missing or has no address yet (e.g. ethernet before DHCP finishes).
std::string iface_ipv4(const std::string& iface);

// listen= value -> bind address. "" means unresolvable right now.
//   all -> 0.0.0.0, usb -> usb0, ethernet -> eth0,
//   anything else -> explicit IPv4 address or interface name.
std::string resolve_listen(const std::string& listen);
