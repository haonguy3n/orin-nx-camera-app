/*
 * API mimic of folly/io/async/AsyncServerSocket.h (github.com/facebook/
 * folly), implemented from scratch over GLib's GSocketService — mimicked,
 * not copied: none of Meta's implementation is used. Accept callbacks
 * fire on the default GMainContext (our EventBase mimic's loop).
 *
 * Folly API kept: bind(), addAcceptCallback(), startAccepting().
 * Adapted: the accept callback receives the GSocketConnection* (the
 * platform connection type) instead of a raw fd, bind() takes an
 * explicit address and returns folly::Expected instead of throwing.
 * Dropped: multiple callbacks/event bases, connection queueing, pause —
 * add when a caller appears.
 */
#pragma once

#include <gio/gio.h>

#include <functional>
#include <string>

#include "camera/folly/Expected.h"
#include "camera/folly/Unit.h"

namespace folly {

class AsyncServerSocket {
public:
    using AcceptCallback = std::function<void(GSocketConnection*)>;

    AsyncServerSocket() = default;
    ~AsyncServerSocket();

    AsyncServerSocket(const AsyncServerSocket&) = delete;
    AsyncServerSocket& operator=(const AsyncServerSocket&) = delete;

    /// Binds |address|:|port| (TCP). Call once, before startAccepting().
    Expected<Unit, std::string> bind(const std::string& address, int port);

    /// The callback invoked for every accepted connection. The connection
    /// is owned by GIO for the duration of the call; take a ref to keep it.
    void addAcceptCallback(AcceptCallback callback) {
        callback_ = std::move(callback);
    }

    /// Starts accepting on the default GMainContext.
    void startAccepting();

private:
    static gboolean on_incoming(GSocketService* service,
                                GSocketConnection* connection,
                                GObject* source, gpointer user_data);

    GSocketService* service_ = nullptr;
    AcceptCallback callback_;
};

}  // namespace folly
