// TCP control server implementing proto/PROTOCOL.md: newline-delimited JSON
// request/response. Handles only the transport and JSON-RPC envelope; all
// method logic is delegated to IControlHandler instances via ControlRegistry.
#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include <string>
#include <unordered_set>

#include "camera/control/ControlContext.h"
#include "camera/control/ControlHandler.h"
#include "camera/control/ControlRegistry.h"
#include "camera/folly/Expected.h"
#include "camera/folly/Unit.h"
#include "camera/folly/io/async/AsyncServerSocket.h"
#include "camera/folly/io/async/SSLContext.h"

namespace camera {

class ControlServer {
public:
    ControlServer(ControlRegistry& registry, ControlContext context);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Binds |address|:|port| and accepts connections on the default
    // GMainContext. Call once.
    folly::Expected<folly::Unit, std::string> start(
        const std::string& address, int port);

private:
    struct Conn;

    void accept_connection(GSocketConnection* connection);
    static void on_line(GObject* source, GAsyncResult* result,
                        gpointer user_data);
    static void close_conn(Conn* conn);

    void process_line(Conn* conn, const char* line);

    ControlRegistry& registry_;
    ControlContext context_;
    folly::AsyncServerSocket socket_;
    folly::SSLContext tls_;  // disabled unless [server] tls-* is configured
    std::unordered_set<Conn*> conns_;
};

}  // namespace camera
