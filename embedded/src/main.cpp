// camera-streamer -- RTSP streaming service for the Jetson camera device.
//
// Usage: camera-streamer [config-file]
// Default config: DEFAULT_CONF_PATH (/etc/camera-streamer.conf).

#include <gst/gst.h>

#include "core/application.h"

#ifndef DEFAULT_CONF_PATH
#define DEFAULT_CONF_PATH "/etc/camera-streamer.conf"
#endif

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    Application app(argc > 1 ? argv[1] : DEFAULT_CONF_PATH);
    if (!app.start())
        return 1;  // systemd Restart=on-failure retries (e.g. DHCP not up yet)

    return app.run();
}
