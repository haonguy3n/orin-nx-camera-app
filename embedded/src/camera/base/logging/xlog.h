/*
 * API mimic of folly/logging/xlog.h (github.com/facebook/folly),
 * implemented from scratch over GLib logging — mimicked, not copied:
 * none of Meta's implementation is used. XLOGF() keeps folly's level
 * names and file:line capture, routed to g_log (journald under
 * systemd). FATAL aborts, as in folly.
 * Dropped: streaming XLOG(), per-category runtime configuration,
 * rate-limited variants (XLOG_EVERY_N, ...) — add when a caller appears.
 */
#pragma once

#include <glib.h>

namespace camera::base {
namespace detail {

/// Compile-time-capable basename: folly logs "file.cpp:123", not the
/// full path the compiler puts in __FILE__.
constexpr const char* xlogBasename(const char* path) {
    const char* base = path;
    for (const char* c = path; *c; ++c) {
        if (*c == '/')
            base = c + 1;
    }
    return base;
}

}  // namespace detail
}  // namespace camera::base

// folly level -> GLib level. FATAL maps to G_LOG_LEVEL_ERROR, which
// aborts the process — matching folly's XLOGF(FATAL) semantics.
#define XLOG_LEVEL_DBG G_LOG_LEVEL_DEBUG
#define XLOG_LEVEL_INFO G_LOG_LEVEL_MESSAGE
#define XLOG_LEVEL_WARN G_LOG_LEVEL_WARNING
#define XLOG_LEVEL_ERR G_LOG_LEVEL_CRITICAL
#define XLOG_LEVEL_FATAL G_LOG_LEVEL_ERROR

/// printf-style log with source location:
///     XLOGF(INFO, "streamed %lld bytes", total);
#define XLOGF(level, fmt, ...)                                          \
    g_log(G_LOG_DOMAIN, XLOG_LEVEL_##level, "[%s:%d] " fmt,             \
          ::camera::base::detail::xlogBasename(__FILE__), __LINE__,            \
          ##__VA_ARGS__)
