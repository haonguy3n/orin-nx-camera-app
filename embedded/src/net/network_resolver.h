// Resolution of the [server] listen= setting.
//
// Wraps the free-function net_util into a class for consistency with the
// OOP design and for potential future mocking (e.g. testing interface
// resolution logic without real getifaddrs).
#pragma once

#include <string>

class NetworkResolver {
public:
    // First IPv4 address of a network interface, or "" if the interface is
    // missing or has no address yet (e.g. ethernet before DHCP finishes).
    static std::string iface_ipv4(const std::string& iface);

    // listen= value -> bind address. "" means unresolvable right now.
    //   all -> 0.0.0.0, usb -> usb0, ethernet -> eth0,
    //   anything else -> explicit IPv4 address or interface name.
    static std::string resolve_listen(const std::string& listen);
};
