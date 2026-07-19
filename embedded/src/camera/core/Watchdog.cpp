#include "camera/core/Watchdog.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>

#include "camera/base/logging/xlog.h"

namespace camera {

// Minimal sd_notify(3): one datagram to $NOTIFY_SOCKET. Errors are
// ignored -- if systemd isn't listening there is nothing useful to do.
static void sd_notify(const char* state) {
    const char* path = std::getenv("NOTIFY_SOCKET");
    if (!path || !*path)
        return;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (std::strlen(path) >= sizeof(addr.sun_path))
        return;
    std::strcpy(addr.sun_path, path);
    if (path[0] == '@')  // abstract socket namespace
        addr.sun_path[0] = '\0';

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return;
    sendto(fd, state, std::strlen(state), 0,
           reinterpret_cast<sockaddr*>(&addr),
           offsetof(sockaddr_un, sun_path) + std::strlen(path));
    close(fd);
}

void Watchdog::start() {
    sd_notify("READY=1");

    // WATCHDOG_USEC/WATCHDOG_PID are set by systemd from WatchdogSec=.
    const char* usec_str = std::getenv("WATCHDOG_USEC");
    const char* pid_str = std::getenv("WATCHDOG_PID");
    if (!usec_str)
        return;
    if (pid_str && static_cast<pid_t>(std::atol(pid_str)) != getpid())
        return;

    long usec = std::atol(usec_str);
    if (usec <= 0)
        return;

    // Pet at half the timeout, as sd_watchdog_enabled(3) recommends.
    guint interval_ms = static_cast<guint>(usec / 1000 / 2);
    source_id_ = g_timeout_add(
        interval_ms,
        [](gpointer) -> gboolean {
            sd_notify("WATCHDOG=1");
            return G_SOURCE_CONTINUE;
        },
        nullptr);
    XLOGF(INFO, "watchdog: enabled, petting every %u ms", interval_ms);
}

void Watchdog::stop() {
    if (source_id_ == 0)
        return;
    g_source_remove(source_id_);
    source_id_ = 0;
    sd_notify("STOPPING=1");
}

}  // namespace camera
