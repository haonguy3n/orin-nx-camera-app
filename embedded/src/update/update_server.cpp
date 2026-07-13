#include "update/update_server.h"

#include <glib.h>
#include <gio/gio.h>
#include <json-glib/json-glib.h>

#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <string>
#include <thread>

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

/// Writes a JSON line to the connection's output stream.
void send_json(GSocketConnection* conn, const std::string& json) {
    std::string out = json + "\n";
    g_output_stream_write_all(
        g_io_stream_get_output_stream(G_IO_STREAM(conn)),
        out.data(), out.size(), nullptr, nullptr, nullptr);
}

/// Reads exactly one line (up to \n) from the connection's input stream.
/// Returns empty string on EOF/error.
std::string read_line(GSocketConnection* conn) {
    GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
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
void handle_connection(GSocketConnection* conn,
                       SwupdateClient& swupdate) {
    const std::string peer = peer_name(conn);
    g_message("update: %s connected", peer.c_str());

    // GSocketService sets sockets to non-blocking mode, but we do blocking
    // I/O in this thread. Switch back to blocking so g_input_stream_read
    // waits for data instead of returning G_IO_ERROR_WOULD_BLOCK.
    GSocket* sock = g_socket_connection_get_socket(conn);
    if (sock)
        g_socket_set_blocking(sock, TRUE);

    // 1. Read header
    std::string header = read_line(conn);
    if (header.empty()) {
        g_warning("update: %s: no header received", peer.c_str());
        g_object_unref(conn);
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
        send_json(conn, "{\"ok\":false,\"error\":\"invalid or missing size\"}");
        g_io_stream_close(G_IO_STREAM(conn), nullptr, nullptr);
        g_object_unref(conn);
        return;
    }

    g_message("update: %s: streaming %zu bytes to swupdate", peer.c_str(), size);

    // 2. Connect to swupdate IPC and get a streaming fd
    int swu_fd = swupdate.begin_stream_install();
    if (swu_fd < 0) {
        send_json(conn,
                  "{\"ok\":false,\"error\":\"swupdate busy or unavailable\"}");
        g_io_stream_close(G_IO_STREAM(conn), nullptr, nullptr);
        g_object_unref(conn);
        return;
    }

    // 3. Send ACK to host — ready to receive data
    send_json(conn, "{\"ok\":true}");

    // 4. Stream: read from host socket → write directly to swupdate IPC fd.
    //    No temp file — data goes straight to swupdate, which starts
    //    parsing the CPIO stream as chunks arrive.
    GInputStream* in = g_io_stream_get_input_stream(G_IO_STREAM(conn));
    char buf[65536];
    gsize received = 0;
    bool upload_ok = true;
    while (received < size) {
        gsize to_read = std::min(static_cast<gsize>(sizeof(buf)),
                                 size - received);
        gssize n = g_input_stream_read(in, buf, to_read, nullptr, nullptr);
        if (n <= 0) {
            g_warning("update: %s: read error at %zu/%zu bytes", peer.c_str(),
                      received, size);
            upload_ok = false;
            break;
        }

        // Write chunk to swupdate IPC fd
        ssize_t written = 0;
        while (written < n) {
            ssize_t w = write(swu_fd, buf + written, n - written);
            if (w < 0) {
                if (errno == EINTR)
                    continue;
                g_warning("update: %s: swupdate write error: %s",
                          peer.c_str(), strerror(errno));
                upload_ok = false;
                break;
            }
            written += w;
        }
        if (written < n) {
            upload_ok = false;
            break;
        }
        received += n;
    }

    // Close the swupdate IPC fd — this signals end-of-stream to swupdate,
    // which then starts processing the image.
    close(swu_fd);

    if (!upload_ok || received != size) {
        g_warning("update: %s: upload incomplete (%zu/%zu)", peer.c_str(),
                  received, size);
        swupdate.end_stream_install();  // will report failure via status
        send_json(conn, "{\"ok\":false,\"error\":\"upload incomplete\"}");
        g_io_stream_close(G_IO_STREAM(conn), nullptr, nullptr);
        g_object_unref(conn);
        return;
    }

    g_message("update: %s: stream complete (%zu bytes), swupdate installing",
              peer.c_str(), received);

    // 5. Start polling swupdate for installation progress
    swupdate.end_stream_install();

    // 6. Send final response
    send_json(conn, "{\"ok\":true,\"state\":\"installing\"}");

    // 7. Close connection
    g_io_stream_close(G_IO_STREAM(conn), nullptr, nullptr);
    g_object_unref(conn);
}

}  // namespace

UpdateServer::UpdateServer(SwupdateClient& swupdate)
    : swupdate_(swupdate) {}

UpdateServer::~UpdateServer() {
    if (service_ != nullptr) {
        g_socket_service_stop(service_);
        g_socket_listener_close(G_SOCKET_LISTENER(service_));
        g_object_unref(service_);
    }
}

bool UpdateServer::start(const std::string& address, int port) {
    service_ = g_socket_service_new();

    GInetAddress* inet = g_inet_address_new_from_string(address.c_str());
    if (inet == nullptr) {
        g_printerr("update: invalid address %s\n", address.c_str());
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
        g_printerr("update: bind %s:%d failed: %s\n", address.c_str(), port,
                   err->message);
        g_error_free(err);
        g_object_unref(service_);
        service_ = nullptr;
        return false;
    }

    g_signal_connect(service_, "incoming", G_CALLBACK(on_incoming), this);
    g_socket_service_start(service_);
    g_message("update server listening on %s:%d", address.c_str(), port);
    return true;
}

gboolean UpdateServer::on_incoming(GSocketService* /*service*/,
                                   GSocketConnection* connection,
                                   GObject* /*source*/, gpointer user_data) {
    auto* self = static_cast<UpdateServer*>(user_data);
    // Hold a ref for the thread; it unrefs when done.
    GSocketConnection* conn = static_cast<GSocketConnection*>(
        g_object_ref(connection));
    // Detached thread: handles the full upload lifecycle.
    std::thread(handle_connection, conn, std::ref(self->swupdate_)).detach();
    return FALSE;
}
