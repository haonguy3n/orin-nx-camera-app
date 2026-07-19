#include "camera/update/UpdateServer.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <unistd.h>

#include <thread>

#include "camera/base/File.h"
#include "camera/base/ScopeGuard.h"
#include "camera/base/logging/xlog.h"
#include "proto/Protocol.h"

namespace camera {

namespace {

constexpr size_t kMaxUpdateSize = 2ULL * 1024 * 1024 * 1024;  // 2 GB

std::string peer_name(GSocketConnection* conn) {
    GSocketAddress* addr =
        g_socket_connection_get_remote_address(conn, nullptr);
    std::string out = "unknown";
    if (addr && G_IS_INET_SOCKET_ADDRESS(addr)) {
        auto* isa = G_INET_SOCKET_ADDRESS(addr);
        gchar* ip = g_inet_address_to_string(
            g_inet_socket_address_get_address(isa));
        out = std::string(ip) + ":" +
              std::to_string(g_inet_socket_address_get_port(isa));
        g_free(ip);
    }
    if (addr)
        g_object_unref(addr);
    return out;
}

/// Writes a JSON line to the stream.
void send_json(GIOStream* io, const std::string& json) {
    std::string out = json + "\n";
    g_output_stream_write_all(
        g_io_stream_get_output_stream(io),
        out.data(), out.size(), nullptr, nullptr, nullptr);
}

/// Reads exactly one line (up to \n) from the stream.
/// Returns empty string on EOF/error.
std::string read_line(GIOStream* io) {
    GInputStream* in = g_io_stream_get_input_stream(io);
    std::string line;
    char ch;
    while (true) {
        gssize n = g_input_stream_read(in, &ch, 1, nullptr, nullptr);
        if (n <= 0)
            return line;
        if (ch == '\n')
            return line;
        line += ch;
        if (line.size() > 4096)  // header line sanity limit
            return line;
    }
}

/// Handles one upload connection in its own thread. Protocol:
///   1. Read JSON header: {"size": N}\n
///   2. Write ACK: {"ok": true}\n  (or error)
///   3. Stream N raw bytes directly to swupdate IPC (no temp file)
///   4. Write final response: {"ok": true, "state": "installing"}\n
void handle_connection(GSocketConnection* conn, GIOStream* io,
                       SwupdateClient& swupdate) {
    const std::string peer = peer_name(conn);
    XLOGF(INFO, "update: %s connected", peer.c_str());

    // One cleanup for every exit below. Closing |io| first sends the TLS
    // close_notify when TLS is on; closing the socket stream is then a
    // no-op for plaintext (io == conn stream) and a hard stop otherwise.
    SCOPE_EXIT {
        g_io_stream_close(io, nullptr, nullptr);
        g_io_stream_close(G_IO_STREAM(conn), nullptr, nullptr);
        g_object_unref(io);
        g_object_unref(conn);
    };

    // GSocketService sets sockets to non-blocking mode, but we do blocking
    // I/O in this thread. Switch back to blocking so g_input_stream_read
    // waits for data instead of returning G_IO_ERROR_WOULD_BLOCK.
    GSocket* sock = g_socket_connection_get_socket(conn);
    if (sock)
        g_socket_set_blocking(sock, TRUE);

    // 1. Read header (with TLS this read performs the handshake, including
    // client-certificate verification — an unauthorized client dies here)
    std::string header = read_line(io);
    if (header.empty()) {
        XLOGF(WARN, "update: %s: no header received", peer.c_str());
        return;
    }

    // Parse {"size": N}
    gsize size = 0;
    bool ok = false;
    JsonParser* parser = json_parser_new();
    GError* perr = nullptr;
    if (json_parser_load_from_data(parser, header.c_str(), -1, &perr)) {
        JsonNode* root = json_parser_get_root(parser);
        if (root && JSON_NODE_HOLDS_OBJECT(root)) {
            JsonObject* obj = json_node_get_object(root);
            if (json_object_has_member(obj, "size")) {
                size = static_cast<gsize>(
                    json_object_get_int_member(obj, "size"));
                ok = size > 0 && size <= kMaxUpdateSize;
            }
        }
    }
    if (perr)
        g_error_free(perr);
    g_object_unref(parser);

    if (!ok) {
        send_json(io, "{\"ok\":false,\"error\":\"invalid or missing size\"}");
        return;
    }

    XLOGF(INFO, "update: %s: streaming %zu bytes to swupdate", peer.c_str(), size);

    // 2. Connect to swupdate IPC and get a streaming fd
    auto swu = swupdate.begin_stream_install();
    if (!swu) {
        send_json(io,
                  "{\"ok\":false,\"error\":\"swupdate busy or unavailable\"}");
        return;
    }
    camera::base::File swu_fd = std::move(*swu);

    // 3. Send ACK to host — ready to receive data
    send_json(io, "{\"ok\":true}");

    // 4. Stream: read from host socket → write directly to swupdate IPC fd.
    //    No temp file — data goes straight to swupdate, which starts
    //    parsing the CPIO stream as chunks arrive.
    GInputStream* in = g_io_stream_get_input_stream(io);
    char buf[65536];
    gsize received = 0;
    bool upload_ok = true;
    while (received < size) {
        gsize to_read = std::min(static_cast<gsize>(sizeof(buf)),
                                 size - received);
        gssize n = g_input_stream_read(in, buf, to_read, nullptr, nullptr);
        if (n <= 0) {
            XLOGF(WARN, "update: %s: read error at %zu/%zu bytes", peer.c_str(),
                      received, size);
            upload_ok = false;
            break;
        }

        // Write chunk to swupdate IPC fd (SIGPIPE-safe: swupdate may die
        // mid-stream on a bad image; EPIPE must not kill the process)
        if (write_full_nosigpipe(swu_fd.fd(), buf, n) < 0) {
            XLOGF(WARN, "update: %s: swupdate write error: %s",
                      peer.c_str(), strerror(errno));
            upload_ok = false;
            break;
        }
        received += n;
    }

    // Close the swupdate IPC fd — this signals end-of-stream to swupdate,
    // which then starts processing the image.
    swu_fd.close();

    if (!upload_ok || received != size) {
        XLOGF(WARN, "update: %s: upload incomplete (%zu/%zu)", peer.c_str(),
                  received, size);
        swupdate.end_stream_install();  // will report failure via status
        send_json(io, "{\"ok\":false,\"error\":\"upload incomplete\"}");
        return;
    }

    XLOGF(INFO, "update: %s: stream complete (%zu bytes), swupdate installing",
              peer.c_str(), received);

    // 5. Start polling swupdate for installation progress
    swupdate.end_stream_install();

    // 6. Send final response; the SCOPE_EXIT closes the connection
    send_json(io, std::string("{\"ok\":true,\"state\":\"") +
                      proto::update_states::kInstalling + "\"}");
}

}  // namespace

UpdateServer::UpdateServer(SwupdateClient& swupdate, const Config& config)
    : swupdate_(swupdate), config_(config) {}

UpdateServer::~UpdateServer() = default;

camera::base::Expected<camera::base::Unit, std::string> UpdateServer::start(
    const std::string& address, int port) {
    auto tls = camera::base::SSLContext::create(config_.tls_cert, config_.tls_key,
                                         config_.tls_ca);
    if (!tls)
        return camera::base::makeUnexpected("update: " + tls.error());
    tls_ = std::move(*tls);

    if (auto r = socket_.bind(address, port); !r)
        return camera::base::makeUnexpected("update: " + r.error());
    socket_.addAcceptCallback(
        [this](GSocketConnection* connection) { accept_connection(connection); });
    socket_.startAccepting();
    XLOGF(INFO, "update server listening on %s:%d", address.c_str(), port);
    return camera::base::unit;
}

bool UpdateServer::adopt_fd(int fd) {
    GError* failure = nullptr;
    GSocket* socket = g_socket_new_from_fd(fd, &failure);
    if (socket == nullptr) {
        XLOGF(WARN, "update: cannot adopt fd: %s",
              failure != nullptr ? failure->message : "unknown");
        if (failure != nullptr) g_error_free(failure);
        close(fd);
        return false;
    }
    GSocketConnection* conn = g_socket_connection_factory_create_connection(socket);
    g_object_unref(socket);  // the connection holds its own ref
    if (conn == nullptr) {
        XLOGF(WARN, "update: cannot wrap adopted fd");
        return false;
    }
    std::thread(handle_connection, conn, G_IO_STREAM(g_object_ref(conn)),
                std::ref(swupdate_))
        .detach();
    return true;
}

void UpdateServer::accept_connection(GSocketConnection* connection) {
    GIOStream* io = nullptr;
    if (tls_.enabled()) {
        auto wrapped = tls_.wrapServerConnection(G_IO_STREAM(connection));
        if (!wrapped) {
            XLOGF(WARN, "update: %s", wrapped.error().c_str());
            return;
        }
        io = *wrapped;  // handshake happens on the thread's first read
    } else {
        io = G_IO_STREAM(g_object_ref(connection));
    }

    // Hold refs for the thread; it unrefs when done.
    GSocketConnection* conn = static_cast<GSocketConnection*>(
        g_object_ref(connection));
    // Detached thread: handles the full upload lifecycle.
    std::thread(handle_connection, conn, io, std::ref(swupdate_)).detach();
}

}  // namespace camera
