// Binary file upload server for OTA updates.
//
// Accepts .swu file uploads from the host UI over a dedicated TCP port
// (default 8557, configurable via [server] update-port). The protocol is
// simple:
//
//   1. Host connects to the update port
//   2. Host sends a JSON header line: {"size":N}\n
//   3. Device responds: {"ok":true}\n  or  {"ok":false,"error":"..."}\n
//   4. Host streams exactly N raw bytes of the .swu file
//   5. Device streams the bytes straight into the swupdate IPC socket
//      (no temp file); swupdate installs as data arrives
//   6. Device responds: {"ok":true,"state":"installing"}\n
//      or {"ok":false,"error":"..."}\n
//   7. Host polls get-update-status on the control channel for progress
//
// The upload is on a separate port (not the control channel) because the
// control channel is newline-delimited JSON — binary data would break
// framing. One connection = one upload; the connection closes after the
// final response.
#pragma once

#include <gio/gio.h>

#include <string>

#include "camera/config/Config.h"
#include "camera/update/SwupdateClient.h"
#include "camera/base/Expected.h"
#include "camera/base/Unit.h"
#include "camera/base/io/async/AsyncServerSocket.h"
#include "camera/base/io/async/SSLContext.h"

namespace camera {

class UpdateServer {
public:
    UpdateServer(SwupdateClient& swupdate, const Config& config);
    ~UpdateServer();

    UpdateServer(const UpdateServer&) = delete;
    UpdateServer& operator=(const UpdateServer&) = delete;

    /// Binds |address|:|port| and accepts upload connections. Each
    /// connection is handled in its own thread (binary upload is blocking).
    /// Returns the failure reason on bind/TLS-config error.
    camera::base::Expected<camera::base::Unit, std::string> start(
        const std::string& address, int port);

private:
    void accept_connection(GSocketConnection* connection);

    SwupdateClient& swupdate_;
    const Config& config_;
    camera::base::SSLContext tls_;  // disabled unless [server] tls-* is configured
    camera::base::AsyncServerSocket socket_;
};

}  // namespace camera
