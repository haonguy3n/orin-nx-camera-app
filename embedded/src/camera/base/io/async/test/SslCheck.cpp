// Loopback check for camera::base::SSLContext (the "secure USB" channel wrapper).
// Requires the GIO TLS backend (glib-networking) and the openssl CLI.
// Run from embedded/ (one line):
//   g++ -std=c++17 -Wall -Wextra -I src src/camera/base/io/async/SSLContext.cpp
//   src/camera/base/io/async/test/SslCheck.cpp $(pkg-config --cflags --libs
//   gio-2.0) -o /tmp/ssl_check && /tmp/ssl_check
#include <gio/gio.h>

#include <cassert>
#include <cstdlib>
#include <string>
#include <thread>

#include "camera/base/io/async/SSLContext.h"

namespace {

GSocketConnection* connect_client(guint16 port) {
    GSocketClient* client = g_socket_client_new();
    GSocketConnection* conn = g_socket_client_connect_to_host(
        client, "127.0.0.1", port, nullptr, nullptr);
    g_object_unref(client);
    return conn;
}

// Client-side TLS wrap that trusts anything (test client only; the
// device code under test is the server side).
GIOStream* client_tls(GSocketConnection* conn) {
    GIOStream* tls = G_IO_STREAM(
        g_tls_client_connection_new(G_IO_STREAM(conn), nullptr, nullptr));
    assert(tls != nullptr);
    g_tls_client_connection_set_validation_flags(
        G_TLS_CLIENT_CONNECTION(tls), static_cast<GTlsCertificateFlags>(0));
    return tls;
}

}  // namespace

int main() {
    if (!g_tls_backend_supports_tls(g_tls_backend_get_default())) {
        g_message("ssl_check: no GIO TLS backend, skipping");
        return 77;
    }

    char tmpl[] = "/tmp/tls-check-XXXXXX";
    const std::string dir = mkdtemp(tmpl);
    const std::string crt = dir + "/server.crt";
    const std::string key = dir + "/server.key";
    assert(system(("openssl req -x509 -newkey ec -pkeyopt "
                   "ec_paramgen_curve:P-256 -keyout " + key + " -out " + crt +
                   " -days 1 -nodes -subj /CN=tls-check 2>/dev/null")
                      .c_str()) == 0);

    // Unconfigured -> disabled; half-configured -> hard error.
    auto off = camera::base::SSLContext::create("", "", "");
    assert(off.hasValue() && !off->enabled());
    assert(!camera::base::SSLContext::create(crt, "", "").hasValue());
    assert(!camera::base::SSLContext::create(crt, key, dir + "/nope.crt").hasValue());

    GSocketListener* listener = g_socket_listener_new();
    GError* err = nullptr;
    guint16 port =
        g_socket_listener_add_any_inet_port(listener, nullptr, &err);
    assert(port != 0);

    // Case A: server TLS on, no client auth — data round-trips encrypted.
    auto ctx = camera::base::SSLContext::create(crt, key, "");
    assert(ctx.hasValue() && ctx->enabled());
    std::thread server_a([&] {
        GSocketConnection* sc =
            g_socket_listener_accept(listener, nullptr, nullptr, nullptr);
        assert(sc != nullptr);
        auto io = ctx->wrapServerConnection(G_IO_STREAM(sc));
        assert(io.hasValue());
        char buf[16] = {};
        gssize n = g_input_stream_read(g_io_stream_get_input_stream(*io),
                                       buf, sizeof(buf), nullptr, nullptr);
        assert(n > 0);
        g_output_stream_write_all(g_io_stream_get_output_stream(*io), buf,
                                  static_cast<gsize>(n), nullptr, nullptr,
                                  nullptr);
        g_io_stream_close(*io, nullptr, nullptr);
        g_object_unref(*io);
        g_object_unref(sc);
    });
    GSocketConnection* ca_conn = connect_client(port);
    assert(ca_conn != nullptr);
    GIOStream* ca_tls = client_tls(ca_conn);
    assert(g_output_stream_write_all(g_io_stream_get_output_stream(ca_tls),
                                     "ping", 4, nullptr, nullptr, nullptr));
    char in[8] = {};
    assert(g_input_stream_read(g_io_stream_get_input_stream(ca_tls), in,
                               sizeof(in), nullptr, nullptr) == 4);
    assert(std::string(in, 4) == "ping");
    server_a.join();
    g_io_stream_close(ca_tls, nullptr, nullptr);
    g_object_unref(ca_tls);
    g_object_unref(ca_conn);

    // Case B: mTLS required, client presents no certificate — the server
    // must reject the handshake and never see application data.
    auto mctx = camera::base::SSLContext::create(crt, key, crt);
    assert(mctx.hasValue() && mctx->enabled());
    bool server_saw_data = false;
    std::thread server_b([&] {
        GSocketConnection* sc =
            g_socket_listener_accept(listener, nullptr, nullptr, nullptr);
        assert(sc != nullptr);
        auto io = mctx->wrapServerConnection(G_IO_STREAM(sc));
        assert(io.hasValue());
        char buf[8];
        gssize n = g_input_stream_read(g_io_stream_get_input_stream(*io),
                                       buf, sizeof(buf), nullptr, nullptr);
        server_saw_data = n > 0;
        g_io_stream_close(*io, nullptr, nullptr);
        g_object_unref(*io);
        g_io_stream_close(G_IO_STREAM(sc), nullptr, nullptr);
        g_object_unref(sc);
    });
    GSocketConnection* cb_conn = connect_client(port);
    assert(cb_conn != nullptr);
    GIOStream* cb_tls = client_tls(cb_conn);
    // Write may locally succeed; the read must fail once the server
    // aborts the handshake for the missing client certificate.
    g_output_stream_write_all(g_io_stream_get_output_stream(cb_tls), "ping",
                              4, nullptr, nullptr, nullptr);
    g_input_stream_read(g_io_stream_get_input_stream(cb_tls), in, sizeof(in),
                        nullptr, nullptr);
    server_b.join();
    assert(!server_saw_data);
    g_io_stream_close(cb_tls, nullptr, nullptr);
    g_object_unref(cb_tls);
    g_object_unref(cb_conn);
    g_object_unref(listener);

    g_message("ssl_check: OK (echo over TLS; unauthenticated client "
              "rejected under mTLS)");
    return 0;
}
