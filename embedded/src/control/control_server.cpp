#include "control/control_server.h"

#include <string>

#include "control/json_util.h"

namespace {

std::string peer_name(GSocketConnection* connection) {
    GSocketAddress* addr =
        g_socket_connection_get_remote_address(connection, nullptr);
    std::string out = "unknown";
    if (addr != nullptr && G_IS_INET_SOCKET_ADDRESS(addr)) {
        auto* isa = G_INET_SOCKET_ADDRESS(addr);
        gchar* ip = g_inet_address_to_string(
            g_inet_socket_address_get_address(isa));
        out = std::string(ip) + ":" +
              std::to_string(g_inet_socket_address_get_port(isa));
        g_free(ip);
    }
    if (addr != nullptr)
        g_object_unref(addr);
    return out;
}

}  // namespace

// One accepted client connection. Owns refs on the socket/streams; exactly
// one read_line_async is always pending, and its completion callback frees
// the Conn once the connection is done (or the server is gone).
struct ControlServer::Conn {
    ControlServer* server;  // nulled by ~ControlServer
    GSocketConnection* socket;
    GDataInputStream* in;
    GCancellable* cancellable;
    std::string peer;
};

ControlServer::ControlServer(ControlRegistry& registry, ControlContext context)
    : registry_(registry), context_(std::move(context)) {}

ControlServer::~ControlServer() {
    // Orphan live connections; their pending reads complete (cancelled) and
    // free them. Single-threaded: no completion can run during this loop.
    for (Conn* conn : conns_) {
        conn->server = nullptr;
        g_cancellable_cancel(conn->cancellable);
        g_io_stream_close(G_IO_STREAM(conn->socket), nullptr, nullptr);
    }
    conns_.clear();
    if (service_ != nullptr) {
        g_socket_service_stop(service_);
        g_socket_listener_close(G_SOCKET_LISTENER(service_));
        g_object_unref(service_);
    }
}

bool ControlServer::start(const std::string& address, int port) {
    service_ = g_socket_service_new();

    GInetAddress* inet = g_inet_address_new_from_string(address.c_str());
    if (inet == nullptr) {
        g_printerr("control: invalid address %s\n", address.c_str());
        return false;
    }
    GSocketAddress* sockaddr = g_inet_socket_address_new(inet, port);
    g_object_unref(inet);

    GError* err = nullptr;
    gboolean ok = g_socket_listener_add_address(
        G_SOCKET_LISTENER(service_), sockaddr, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_TCP, nullptr, nullptr, &err);
    g_object_unref(sockaddr);
    if (!ok) {
        g_printerr("control: bind %s:%d failed: %s\n", address.c_str(), port,
                   err->message);
        g_error_free(err);
        return false;
    }

    g_signal_connect(service_, "incoming", G_CALLBACK(on_incoming), this);
    g_socket_service_start(service_);
    g_message("control server listening on %s:%d", address.c_str(), port);
    return true;
}

gboolean ControlServer::on_incoming(GSocketService* /*service*/,
                                    GSocketConnection* connection,
                                    GObject* /*source*/, gpointer user_data) {
    auto* self = static_cast<ControlServer*>(user_data);

    auto* conn = new Conn;
    conn->server = self;
    conn->socket = static_cast<GSocketConnection*>(g_object_ref(connection));
    conn->in = g_data_input_stream_new(
        g_io_stream_get_input_stream(G_IO_STREAM(connection)));
    g_data_input_stream_set_newline_type(conn->in,
                                         G_DATA_STREAM_NEWLINE_TYPE_ANY);
    conn->cancellable = g_cancellable_new();
    conn->peer = peer_name(connection);
    self->conns_.insert(conn);

    g_message("control: %s connected", conn->peer.c_str());
    g_data_input_stream_read_line_async(conn->in, G_PRIORITY_DEFAULT,
                                        conn->cancellable, on_line, conn);
    return FALSE;
}

void ControlServer::close_conn(Conn* conn) {
    if (conn->server != nullptr) {
        conn->server->conns_.erase(conn);
        g_message("control: %s disconnected", conn->peer.c_str());
    }
    g_io_stream_close(G_IO_STREAM(conn->socket), nullptr, nullptr);
    g_object_unref(conn->in);
    g_object_unref(conn->cancellable);
    g_object_unref(conn->socket);
    delete conn;
}

void ControlServer::on_line(GObject* source, GAsyncResult* result,
                            gpointer user_data) {
    auto* conn = static_cast<Conn*>(user_data);
    GError* err = nullptr;
    char* line = g_data_input_stream_read_line_finish(
        G_DATA_INPUT_STREAM(source), result, nullptr, &err);
    if (err != nullptr)
        g_error_free(err);

    if (conn->server == nullptr || line == nullptr) {  // gone / EOF / error
        g_free(line);
        close_conn(conn);
        return;
    }

    conn->server->process_line(conn, line);
    g_free(line);
    g_data_input_stream_read_line_async(conn->in, G_PRIORITY_DEFAULT,
                                        conn->cancellable, on_line, conn);
}

void ControlServer::process_line(Conn* conn, const char* line) {
    if (*line == '\0')
        return;  // ignore blank lines (nc users)

    JsonNode* id = nullptr;      // borrowed from the parsed tree
    JsonNode* res = nullptr;     // transfer full
    int err_code = 0;
    std::string err_msg;

    JsonParser* parser = json_parser_new();
    GError* perr = nullptr;
    if (!json_parser_load_from_data(parser, line, -1, &perr)) {
        err_code = kParseError;
        err_msg = perr->message;
        g_error_free(perr);
    } else {
        JsonNode* root = json_parser_get_root(parser);
        if (root == nullptr || !JSON_NODE_HOLDS_OBJECT(root)) {
            err_code = kInvalidRequest;
            err_msg = "request must be a JSON object";
        } else {
            JsonObject* req = json_node_get_object(root);
            if (json_object_has_member(req, "id"))
                id = json_object_get_member(req, "id");
            JsonObject* params = nullptr;
            if (json_object_has_member(req, "params")) {
                JsonNode* p = json_object_get_member(req, "params");
                if (JSON_NODE_HOLDS_OBJECT(p))
                    params = json_node_get_object(p);
            }
            const char* method =
                json_object_has_member(req, "method")
                    ? json_object_get_string_member_with_default(req, "method",
                                                                 nullptr)
                    : nullptr;
            if (method == nullptr) {
                err_code = kInvalidRequest;
                err_msg = "missing method";
            } else {
                IControlHandler* handler = registry_.find(method);
                if (handler == nullptr) {
                    err_code = kUnknownMethod;
                    err_msg = "unknown method '" + std::string(method) + "'";
                } else {
                    res = handler->handle(params, context_, &err_code,
                                          &err_msg);
                }
            }
        }
    }

    // Envelope: {"id": ..., "result": ...} or {"id": ..., "error": {...}}.
    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "id");
    json_builder_add_value(b, id != nullptr ? json_node_copy(id)
                                            : json_node_new(JSON_NODE_NULL));
    if (res != nullptr) {
        json_builder_set_member_name(b, "result");
        json_builder_add_value(b, res);  // ownership transferred
    } else {
        json_builder_set_member_name(b, "error");
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "code");
        json_builder_add_int_value(b, err_code);
        json_builder_set_member_name(b, "message");
        json_builder_add_string_value(b, err_msg.c_str());
        json_builder_end_object(b);
    }
    json_builder_end_object(b);

    JsonNode* reply = take_root(b);
    const std::string out = node_to_string(reply) + "\n";
    json_node_unref(reply);
    g_object_unref(parser);

    g_output_stream_write_all(
        g_io_stream_get_output_stream(G_IO_STREAM(conn->socket)), out.data(),
        out.size(), nullptr, nullptr, nullptr);
}
