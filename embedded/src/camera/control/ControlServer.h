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
#include "camera/base/Expected.h"
#include "camera/base/Unit.h"
#include "camera/base/io/async/AsyncServerSocket.h"
#include "camera/base/io/async/SSLContext.h"

namespace camera {

// Runs one request line through the handlers and returns the reply line
// (empty for a blank request). Exposed so a transport can dispatch in-process:
// the secure USB path used to re-serialise every decrypted request onto a TCP
// socket to 127.0.0.1:8555 just to reach these same handlers.
//
// Handlers touch the config and live GStreamer elements, so this MUST be
// called on the GLib main loop -- see Application's dispatcher, which marshals
// onto it. The TCP server satisfied that incidentally (GSocket callbacks land
// there); an in-process caller has to do it deliberately.
std::string dispatch_request(ControlRegistry& registry, ControlContext& context,
                             const char* line);

class ControlServer {
public:
    // Pushes one line to every connected client, unsolicited. Face detection
    // in network mode needs this: there is no Meta channel without a secure
    // session, so boxes ride the control connection instead.
    //
    // This makes the protocol no longer strictly request/response. Events
    // carry no "id", which is how a client tells them from a reply -- see
    // proto/PROTOCOL.md. Must be called on the GLib main loop.
    void broadcast(const std::string& line);

    ControlServer(ControlRegistry& registry, ControlContext context);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Binds |address|:|port| and accepts connections on the default
    // GMainContext. Call once.
    camera::base::Expected<camera::base::Unit, std::string> start(
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
    camera::base::AsyncServerSocket socket_;
    camera::base::SSLContext tls_;  // disabled unless [server] tls-* is configured
    std::unordered_set<Conn*> conns_;

};

}  // namespace camera
