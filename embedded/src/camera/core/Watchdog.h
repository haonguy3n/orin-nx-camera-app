// Watchdog: systemd notify + watchdog integration.
//
// When run under systemd with Type=notify and WatchdogSec=, this signals
// READY=1 once started and pets the watchdog from the GMainLoop at half
// the configured interval. If the main loop wedges, the pets stop and
// systemd kills/restarts the service. Outside systemd (dev runs) every
// call is a no-op.
//
// sd_notify is hand-rolled (a datagram to $NOTIFY_SOCKET) so we don't
// need libsystemd at build time.
#pragma once

#include <glib.h>

namespace camera {

class Watchdog {
public:
    // Sends READY=1 and, if WATCHDOG_USEC is set for this process,
    // starts the periodic pet on the default GMainContext.
    void start();

    // Sends STOPPING=1 and stops petting.
    void stop();

    ~Watchdog() { stop(); }

private:
    guint source_id_ = 0;
};

}  // namespace camera
