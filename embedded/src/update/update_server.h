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
//   5. Device saves to /tmp/update.swu and auto-triggers swupdate
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

#include "update/swupdate_client.h"

class UpdateServer {
public:
    explicit UpdateServer(SwupdateClient& swupdate);
    ~UpdateServer();

    UpdateServer(const UpdateServer&) = delete;
    UpdateServer& operator=(const UpdateServer&) = delete;

    /// Binds |address|:|port| and accepts upload connections. Each
    /// connection is handled in its own thread (binary upload is blocking).
    /// Returns false on bind failure.
    bool start(const std::string& address, int port);

private:
    static gboolean on_incoming(GSocketService* service,
                                GSocketConnection* connection,
                                GObject* source, gpointer user_data);

    SwupdateClient& swupdate_;
    GSocketService* service_ = nullptr;
};
