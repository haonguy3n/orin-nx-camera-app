// UDP discovery responder (proto/PROTOCOL.md "Discovery"): answers
// {"method":"discover"} broadcasts with device/port info, so the host UI
// can find cameras instead of assuming 192.168.55.1.
#pragma once

#include <gio/gio.h>

#include "camera/config/Config.h"
#include "camera/base/Expected.h"
#include "camera/base/Unit.h"

namespace camera {

class DiscoveryServer {
public:
    explicit DiscoveryServer(const Config& config);
    ~DiscoveryServer();

    DiscoveryServer(const DiscoveryServer&) = delete;
    DiscoveryServer& operator=(const DiscoveryServer&) = delete;

    // Binds 0.0.0.0:discovery-port (broadcasts don't arrive on a specific
    // address, and the reply only reveals what a port scan would).
    camera::base::Expected<camera::base::Unit, std::string> start();

private:
    static gboolean on_datagram(GSocket* socket, GIOCondition condition,
                                gpointer user_data);

    const Config& config_;
    GSocket* socket_ = nullptr;
    GSource* source_ = nullptr;
};

}  // namespace camera
