// Loopback check for the camera::base::EventBase and camera::base::AsyncServerSocket
// mimics (GLib-backed).
// Run from embedded/ (one line):
//   g++ -std=c++17 -Wall -Wextra -I src src/camera/base/io/async/AsyncServerSocket.cpp
//   src/camera/base/io/async/test/IoCheck.cpp $(pkg-config --cflags --libs
//   gio-2.0) -o /tmp/io_check && /tmp/io_check
#include <gio/gio.h>

#include <cassert>
#include <memory>
#include <string>
#include <thread>

#include "camera/base/io/async/AsyncServerSocket.h"
#include "camera/base/io/async/EventBase.h"

int main() {
    camera::base::EventBase evb;

    // runInLoop and runAfterDelay execute in order; terminateLoopSoon
    // makes loopForever return.
    int step = 0;
    evb.runInLoop([&] {
        assert(step == 0);
        step = 1;
    });
    evb.runAfterDelay(
        [&] {
            assert(step == 1);
            step = 2;
            evb.terminateLoopSoon();
        },
        10);
    evb.loopForever();
    assert(step == 2);

    // The closure handed to runInLoop is destroyed exactly once after it
    // runs (the GDestroyNotify path; invoke() itself no longer deletes).
    auto alive = std::make_shared<int>(1);
    std::weak_ptr<int> watch = alive;
    evb.runInLoop([keep = std::move(alive)] { (void)keep; });
    evb.runInLoop([&] { evb.terminateLoopSoon(); });
    evb.loopForever();
    assert(watch.expired());

    // AsyncServerSocket: accept fires on the loop; echo one message.
    camera::base::AsyncServerSocket server;
    int port = 0;
    for (int candidate = 45871; candidate < 45971; ++candidate) {
        if (server.bind("127.0.0.1", candidate)) {
            port = candidate;
            break;
        }
    }
    assert(port != 0);
    camera::base::AsyncServerSocket conflict;
    assert(!conflict.bind("127.0.0.1", port));  // port taken -> must fail

    bool accepted = false;
    server.addAcceptCallback([&](GSocketConnection* conn) {
        accepted = true;
        GSocket* sock = g_socket_connection_get_socket(conn);
        g_socket_set_blocking(sock, TRUE);  // check does blocking echo
        char buf[16] = {};
        gssize n = g_input_stream_read(
            g_io_stream_get_input_stream(G_IO_STREAM(conn)), buf,
            sizeof(buf), nullptr, nullptr);
        assert(n > 0);
        g_output_stream_write_all(
            g_io_stream_get_output_stream(G_IO_STREAM(conn)), buf,
            static_cast<gsize>(n), nullptr, nullptr, nullptr);
        evb.terminateLoopSoon();
    });
    server.startAccepting();

    std::thread client([&] {
        GSocketClient* c = g_socket_client_new();
        GSocketConnection* conn = g_socket_client_connect_to_host(
            c, "127.0.0.1", static_cast<guint16>(port), nullptr, nullptr);
        assert(conn != nullptr);
        assert(g_output_stream_write_all(
            g_io_stream_get_output_stream(G_IO_STREAM(conn)), "ping", 4,
            nullptr, nullptr, nullptr));
        char in[8] = {};
        assert(g_input_stream_read(
                   g_io_stream_get_input_stream(G_IO_STREAM(conn)), in,
                   sizeof(in), nullptr, nullptr) == 4);
        assert(std::string(in, 4) == "ping");
        g_io_stream_close(G_IO_STREAM(conn), nullptr, nullptr);
        g_object_unref(conn);
        g_object_unref(c);
    });
    evb.loopForever();
    client.join();
    assert(accepted);

    g_message("io_check: OK (EventBase ordering + AsyncServerSocket echo)");
    return 0;
}
