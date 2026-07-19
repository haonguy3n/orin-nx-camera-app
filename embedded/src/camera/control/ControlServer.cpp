#include "camera/control/ControlServer.h"

#include <string>

#include "camera/control/JsonUtil.h"

#include "camera/base/logging/xlog.h"

namespace camera {

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
    GIOStream* io;  // the socket stream, or its TLS wrap when enabled
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
}

camera::base::Expected<camera::base::Unit, std::string> ControlServer::start(
    const std::string& address, int port) {
    auto tls = camera::base::SSLContext::create(context_.config.tls_cert,
                                         context_.config.tls_key,
                                         context_.config.tls_ca);
    if (!tls)
        return camera::base::makeUnexpected("control: " + tls.error());
    tls_ = std::move(*tls);

    if (auto r = socket_.bind(address, port); !r)
        return camera::base::makeUnexpected("control: " + r.error());
    socket_.addAcceptCallback(
        [this](GSocketConnection* connection) { accept_connection(connection); });
    socket_.startAccepting();
    XLOGF(INFO, "control server listening on %s:%d", address.c_str(), port);
    return camera::base::unit;
}

void ControlServer::accept_connection(GSocketConnection* connection) {
    GIOStream* io = nullptr;
    if (tls_.enabled()) {
        auto wrapped = tls_.wrapServerConnection(G_IO_STREAM(connection));
        if (!wrapped) {
            XLOGF(WARN, "control: %s", wrapped.error().c_str());
            return;
        }
        io = *wrapped;  // handshake happens on the first read below
    } else {
        io = G_IO_STREAM(g_object_ref(connection));
    }

    auto* conn = new Conn;
    conn->server = this;
    conn->socket = static_cast<GSocketConnection*>(g_object_ref(connection));
    conn->io = io;
    conn->in = g_data_input_stream_new(g_io_stream_get_input_stream(io));
    g_data_input_stream_set_newline_type(conn->in,
                                         G_DATA_STREAM_NEWLINE_TYPE_ANY);
    conn->cancellable = g_cancellable_new();
    conn->peer = peer_name(connection);
    conns_.insert(conn);

    XLOGF(INFO, "control: %s connected", conn->peer.c_str());
    g_data_input_stream_read_line_async(conn->in, G_PRIORITY_DEFAULT,
                                        conn->cancellable, on_line, conn);
}

void ControlServer::close_conn(Conn* conn) {
    if (conn->server != nullptr) {
        conn->server->conns_.erase(conn);
        XLOGF(INFO, "control: %s disconnected", conn->peer.c_str());
    }
    g_io_stream_close(conn->io, nullptr, nullptr);  // TLS: sends close_notify
    g_io_stream_close(G_IO_STREAM(conn->socket), nullptr, nullptr);
    g_object_unref(conn->in);
    g_object_unref(conn->cancellable);
    g_object_unref(conn->io);
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

// Request line -> reply line. Shared by the TCP control server and the
// secure-USB transport, which dispatches in-process instead of looping a
// socket back to 127.0.0.1 just to reach the same handlers.
std::string dispatch_request(ControlRegistry& registry_,
                             ControlContext& context_, const char* line) {
    if (*line == '\0')
        return {};  // blank lines (nc users) get no reply

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
            // Checked by hand: a non-string "method" must yield the normal
            // error reply, not a json-glib g_critical (untrusted input).
            const char* method = nullptr;
            if (json_object_has_member(req, "method")) {
                JsonNode* m = json_object_get_member(req, "method");
                if (JSON_NODE_HOLDS_VALUE(m) &&
                    json_node_get_value_type(m) == G_TYPE_STRING)
                    method = json_node_get_string(m);
            }
            if (method == nullptr) {
                err_code = kInvalidRequest;
                err_msg = "missing method";
            } else {
                IControlHandler* handler = registry_.find(method);
                if (handler == nullptr) {
                    err_code = kUnknownMethod;
                    err_msg = "unknown method '" + std::string(method) + "'";
                } else {
                    HandlerResult r = handler->handle(params, context_);
                    if (r) {
                        res = *r;
                    } else {
                        err_code = r.error().code;
                        err_msg = std::move(r.error().message);
                    }
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

    return out;
}
void ControlServer::broadcast(const std::string& line) {
    if (line.empty() || conns_.empty()) return;
    const std::string out = line.back() == '\n' ? line : line + "\n";
    for (Conn* conn : conns_) {
        // Best-effort: a client that cannot keep up is not worth failing the
        // detector over, and boxes are droppable by nature.
        g_output_stream_write_all(g_io_stream_get_output_stream(conn->io),
                                  out.data(), out.size(), nullptr, nullptr,
                                  nullptr);
    }
}

void ControlServer::process_line(Conn* conn, const char* line) {
    const std::string out = dispatch_request(registry_, context_, line);
    if (out.empty())
        return;
    g_output_stream_write_all(
        g_io_stream_get_output_stream(conn->io), out.data(),
        out.size(), nullptr, nullptr, nullptr);
}

}  // namespace camera
