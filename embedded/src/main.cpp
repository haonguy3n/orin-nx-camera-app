// camera-streamer — RTSP streaming service for the Jetson camera device.
//
// Usage: camera-streamer [config-file]
// Default config: DEFAULT_CONF_PATH (/etc/camera-streamer.conf).

#include <glib-unix.h>
#include <gst/gst.h>

#include <memory>

#include "config.h"
#include "rtsp_server.h"

#ifndef DEFAULT_CONF_PATH
#define DEFAULT_CONF_PATH "/etc/camera-streamer.conf"
#endif

// Owns everything with process lifetime. M2 adds the TCP control server as
// another member alongside `rtsp`.
struct App {
    std::string conf_path;
    Config config;
    std::unique_ptr<RtspServer> rtsp;
    GMainLoop* loop = nullptr;
};

static gboolean on_signal(gpointer user_data) {
    auto* app = static_cast<App*>(user_data);
    g_message("shutdown signal received");
    g_main_loop_quit(app->loop);
    return G_SOURCE_REMOVE;
}

// SIGHUP = reload config and re-serve (systemctl reload camera-streamer).
// This is how the RTSP service switches interface (listen=usb/ethernet/all)
// or pipeline settings at runtime; connected clients are dropped on purpose.
static gboolean on_reload(gpointer user_data) {
    auto* app = static_cast<App*>(user_data);
    g_message("reload: re-reading %s", app->conf_path.c_str());

    Config previous = app->config;
    app->config = load_config(app->conf_path);

    app->rtsp.reset();  // release the port before rebinding
    app->rtsp = std::make_unique<RtspServer>(app->config);
    if (!app->rtsp->start()) {
        g_printerr("reload: new config failed, reverting\n");
        app->config = previous;
        app->rtsp.reset();
        app->rtsp = std::make_unique<RtspServer>(app->config);
        if (!app->rtsp->start()) {
            g_printerr("reload: revert failed too, exiting\n");
            g_main_loop_quit(app->loop);  // systemd restarts us
        }
    }
    return G_SOURCE_CONTINUE;
}

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    App app;
    app.conf_path = argc > 1 ? argv[1] : DEFAULT_CONF_PATH;
    app.config = load_config(app.conf_path);

    app.rtsp = std::make_unique<RtspServer>(app.config);
    if (!app.rtsp->start())
        return 1;  // systemd Restart=on-failure retries (e.g. DHCP not up yet)

    app.loop = g_main_loop_new(nullptr, FALSE);
    g_unix_signal_add(SIGINT, on_signal, &app);
    g_unix_signal_add(SIGTERM, on_signal, &app);
    g_unix_signal_add(SIGHUP, on_reload, &app);

    g_main_loop_run(app.loop);

    g_message("exiting");
    app.rtsp.reset();
    g_main_loop_unref(app.loop);
    gst_deinit();
    return 0;
}
