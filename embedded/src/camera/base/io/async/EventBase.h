/*
 * API mimic of folly/io/async/EventBase.h (github.com/facebook/folly),
 * implemented from scratch over GLib's GMainLoop — mimicked, not copied:
 * none of Meta's implementation is used. The loop is the process-default
 * GMainContext (the one GStreamer and GIO are attached to), not libevent.
 *
 * Folly API kept: loopForever(), terminateLoopSoon(), runInLoop(),
 * runAfterDelay(). Dropped: threads/queues (runInEventBaseThread),
 * HHWheelTimer, keepalive tokens — add when a caller appears.
 */
#pragma once

#include <glib.h>

#include <cstdint>
#include <functional>
#include <utility>

namespace camera::base {

class EventBase {
public:
    using Func = std::function<void()>;

    EventBase() : loop_(g_main_loop_new(nullptr, FALSE)) {}
    ~EventBase() { g_main_loop_unref(loop_); }

    EventBase(const EventBase&) = delete;
    EventBase& operator=(const EventBase&) = delete;

    /// Runs the loop until terminateLoopSoon().
    void loopForever() { g_main_loop_run(loop_); }

    /// Makes loopForever() return. Safe to call from loop callbacks.
    void terminateLoopSoon() { g_main_loop_quit(loop_); }

    /// Runs |fn| on the next loop iteration.
    void runInLoop(Func fn) {
        g_idle_add_full(G_PRIORITY_DEFAULT_IDLE, invoke,
                        new Func(std::move(fn)), destroy);
    }

    /// Runs |fn| once after |milliseconds|.
    void runAfterDelay(Func fn, uint32_t milliseconds) {
        g_timeout_add_full(G_PRIORITY_DEFAULT, milliseconds, invoke,
                           new Func(std::move(fn)), destroy);
    }

private:
    // The Func is deleted by |destroy| when GLib drops the source —
    // after dispatch, on g_source_remove(), or at context teardown —
    // so a source that never runs doesn't leak its closure.
    static gboolean invoke(gpointer data) {
        (*static_cast<Func*>(data))();
        return G_SOURCE_REMOVE;
    }

    static void destroy(gpointer data) { delete static_cast<Func*>(data); }

    GMainLoop* loop_;
};

}  // namespace camera::base
