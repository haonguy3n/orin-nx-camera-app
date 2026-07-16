#include "camera/lib/net/NetworkResolver.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>

namespace camera {

namespace {

// Interface names behind the listen= aliases.
constexpr const char* kUsbIface = "usb0";
constexpr const char* kEthIface = "eth0";

}  // namespace

std::string NetworkResolver::iface_ipv4(const std::string& iface) {
    struct ifaddrs* addrs = nullptr;
    if (getifaddrs(&addrs) != 0)
        return "";
    std::string ip;
    for (struct ifaddrs* a = addrs; a != nullptr; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || iface != a->ifa_name)
            continue;
        char buf[INET_ADDRSTRLEN];
        auto* sin = reinterpret_cast<struct sockaddr_in*>(a->ifa_addr);
        if (inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf))) {
            ip = buf;
            break;
        }
    }
    freeifaddrs(addrs);
    return ip;
}

std::string NetworkResolver::resolve_listen(const std::string& listen) {
    if (listen == "all")
        return "0.0.0.0";
    if (listen == "usb")
        return iface_ipv4(kUsbIface);
    if (listen == "ethernet")
        return iface_ipv4(kEthIface);
    struct in_addr dummy;
    if (inet_pton(AF_INET, listen.c_str(), &dummy) == 1)
        return listen;  // explicit IPv4 address
    return iface_ipv4(listen);  // explicit interface name
}

}  // namespace camera
