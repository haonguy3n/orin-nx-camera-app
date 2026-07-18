#include "camera/base/io/async/AsyncServerSocket.h"

namespace camera::base {

AsyncServerSocket::~AsyncServerSocket() {
    if (service_ != nullptr) {
        // GIO may still hold its own ref with a queued "incoming"
        // emission; disconnect so it can never fire into a dead |this|.
        g_signal_handlers_disconnect_by_data(service_, this);
        g_socket_service_stop(service_);
        g_socket_listener_close(G_SOCKET_LISTENER(service_));
        g_object_unref(service_);
    }
}

Expected<Unit, std::string> AsyncServerSocket::bind(
    const std::string& address, int port) {
    if (service_ == nullptr)  // keep any prior listener on rebind attempts
        service_ = g_socket_service_new();

    GInetAddress* inet = g_inet_address_new_from_string(address.c_str());
    if (inet == nullptr)
        return makeUnexpected("invalid address " + address);
    GSocketAddress* sockaddr = g_inet_socket_address_new(inet, port);
    g_object_unref(inet);

    GError* err = nullptr;
    gboolean ok = g_socket_listener_add_address(
        G_SOCKET_LISTENER(service_), sockaddr, G_SOCKET_TYPE_STREAM,
        G_SOCKET_PROTOCOL_TCP, nullptr, nullptr, &err);
    g_object_unref(sockaddr);
    if (!ok) {
        std::string msg = "bind " + address + ":" + std::to_string(port) +
                          " failed: " + err->message;
        g_error_free(err);
        return makeUnexpected(std::move(msg));
    }
    return unit;
}

void AsyncServerSocket::startAccepting() {
    if (service_ == nullptr)  // bind() never succeeded; nothing to accept on
        return;
    g_signal_connect(service_, "incoming", G_CALLBACK(on_incoming), this);
    g_socket_service_start(service_);
}

gboolean AsyncServerSocket::on_incoming(GSocketService* /*service*/,
                                        GSocketConnection* connection,
                                        GObject* /*source*/,
                                        gpointer user_data) {
    auto* self = static_cast<AsyncServerSocket*>(user_data);
    if (self->callback_)
        self->callback_(connection);
    return FALSE;
}

}  // namespace camera::base
