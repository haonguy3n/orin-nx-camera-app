// TCP control server implementing proto/PROTOCOL.md: newline-delimited JSON
// request/response (exposure, gain, trigger, status, reload).
#pragma once

#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include <functional>
#include <string>
#include <unordered_set>

#include "config.h"

class RtspServer;

// Wiring back into the application, as callbacks so this class doesn't
// depend on main's App (whose members are replaced on reload).
struct ControlHooks {
    std::function<Config&()> config;    // live, mutable configuration
    std::function<RtspServer*()> rtsp;  // current RTSP server
    std::function<void()> reload;       // full config-file reload (= SIGHUP)
};

class ControlServer {
public:
    explicit ControlServer(ControlHooks hooks);
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
    // Returns the result node (transfer full), or nullptr with
    // |err_code|/|err_msg| set.
    JsonNode* dispatch(const char* method, JsonObject* params, int* err_code,
                       std::string* err_msg);

    ControlHooks hooks_;
    GSocketService* service_ = nullptr;
    std::unordered_set<Conn*> conns_;
};
