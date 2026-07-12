// TCP control server implementing proto/PROTOCOL.md: newline-delimited JSON
// request/response. Handles only the transport and JSON-RPC envelope; all
// method logic is delegated to IControlHandler instances via ControlRegistry.
#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include <string>
#include <unordered_set>

#include "control/control_context.h"
#include "control/control_handler.h"
#include "control/control_registry.h"

class ControlServer {
public:
    ControlServer(ControlRegistry& registry, ControlContext context);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // Binds |address|:|port| and accepts connections on the default
    // GMainContext. Call once.
    bool start(const std::string& address, int port);

private:
    struct Conn;

    static gboolean on_incoming(GSocketService* service,
                                GSocketConnection* connection, GObject* source,
                                gpointer user_data);
    static void on_line(GObject* source, GAsyncResult* result,
                        gpointer user_data);
    static void close_conn(Conn* conn);

    void process_line(Conn* conn, const char* line);

    ControlRegistry& registry_;
    ControlContext context_;
    GSocketService* service_ = nullptr;
    std::unordered_set<Conn*> conns_;
};
