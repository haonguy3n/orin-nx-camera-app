// camera-streamer — RTSP streaming service for the Jetson camera device.
//
// Usage: camera-streamer [config-file]
// Default config: DEFAULT_CONF_PATH (/etc/camera-streamer.conf).

#include <glib-unix.h>
#include <gst/gst.h>
#include <unistd.h>

#include <memory>

#include <cstdlib>

#include "config.h"
#include "control_server.h"
#include "discovery_server.h"
#include "isp_file.h"
#include "rtsp_server.h"

#ifndef DEFAULT_CONF_PATH
#define DEFAULT_CONF_PATH "/etc/camera-streamer.conf"
#endif

// Owns everything with process lifetime.
struct App {
    std::string conf_path;
    Config config;
    std::unique_ptr<RtspServer> rtsp;
    std::unique_ptr<ControlServer> control;
    std::unique_ptr<DiscoveryServer> discovery;
    GMainLoop* loop = nullptr;
    int exit_code = 0;
};

static void do_reload(App* app);
static void apply_tuning(App* app);

// Brings up the RTSP server and (unless control-port=0) the control server
// on the same address, from app->config.
static bool start_servers(App* app) {
    app->rtsp = std::make_unique<RtspServer>(app->config);
    app->rtsp->set_stall_handler([]() {
        // Hard exit, no orderly teardown: dismantling GStreamer around a
        // stalled pipeline crashed with SIGBUS on target, and systemd
        // restarts us either way — a wedged process must not linger.
        _exit(1);
    });
    if (!app->rtsp->start()) {
        app->rtsp.reset();
        return false;
    }

    if (app->config.control_port > 0) {
        ControlHooks hooks;
        hooks.config = [app]() -> Config& { return app->config; };
        hooks.rtsp = [app]() { return app->rtsp.get(); };
        hooks.reload = [app]() { do_reload(app); };
        hooks.apply_tuning = [app]() { apply_tuning(app); };
        app->control = std::make_unique<ControlServer>(std::move(hooks));
        if (!app->control->start(app->rtsp->bound_address(),
                                 app->config.control_port)) {
            app->control.reset();
            app->rtsp.reset();
            return false;
        }
    }

    if (app->config.discovery_port > 0) {
        app->discovery = std::make_unique<DiscoveryServer>(app->config);
        // Discovery is a convenience; a bind failure (port taken) is not
        // worth refusing to stream over.
        if (!app->discovery->start())
            app->discovery.reset();
    }
    return true;
}

static void stop_servers(App* app) {
    app->discovery.reset();
    app->control.reset();  // before rtsp: its hooks reach into the App
    app->rtsp.reset();
}

// Reload = re-read the config file and re-serve (systemctl reload, SIGHUP,
// or the control protocol's "reload"). This is how the service switches
// interface (listen=usb/ethernet/all) or pipeline settings at runtime;
// connected clients are dropped on purpose.
static void do_reload(App* app) {
    g_message("reload: re-reading %s", app->conf_path.c_str());

    Config previous = app->config;
    app->config = load_config(app->conf_path);

    stop_servers(app);  // release the ports before rebinding
    if (!start_servers(app)) {
        g_printerr("reload: new config failed, reverting\n");
        app->config = previous;
        stop_servers(app);
        if (!start_servers(app)) {
            g_printerr("reload: revert failed too, exiting\n");
            app->exit_code = 1;
            g_main_loop_quit(app->loop);  // systemd restarts us
        }
    }
}

// Applies changed deep tuning (set-tuning): the overrides file is already
// written; libargus only re-reads it when nvargus-daemon restarts, and a
// daemon restart kills every live Argus session — so drop the pipelines
// around it. ~5 s outage; clients reconnect.
static void apply_tuning(App* app) {
    g_message("tuning: restarting nvargus-daemon and pipelines");
    stop_servers(app);
    if (system("systemctl restart nvargus-daemon 2>/dev/null") != 0)
        g_warning("tuning: nvargus-daemon restart failed (not on target?)");
    if (!start_servers(app)) {
        g_printerr("tuning: restart failed, exiting\n");
        app->exit_code = 1;
        g_main_loop_quit(app->loop);  // systemd restarts us
    }
}

static gboolean on_signal(gpointer user_data) {
    auto* app = static_cast<App*>(user_data);
    g_message("shutdown signal received");
    g_main_loop_quit(app->loop);
    return G_SOURCE_REMOVE;
}

static gboolean on_reload(gpointer user_data) {
    do_reload(static_cast<App*>(user_data));
    return G_SOURCE_CONTINUE;
}

int main(int argc, char* argv[]) {
    gst_init(&argc, &argv);

    App app;
    app.conf_path = argc > 1 ? argv[1] : DEFAULT_CONF_PATH;
    app.config = load_config(app.conf_path);

    // Bring the overrides file in line with [tuning] before any Argus
    // session exists (the daemon caches it at first use).
    isp_file_sync(app.config.tuning);

    if (!start_servers(&app))
        return 1;  // systemd Restart=on-failure retries (e.g. DHCP not up yet)

    app.loop = g_main_loop_new(nullptr, FALSE);
    g_unix_signal_add(SIGINT, on_signal, &app);
    g_unix_signal_add(SIGTERM, on_signal, &app);
    g_unix_signal_add(SIGHUP, on_reload, &app);

    g_main_loop_run(app.loop);

    g_message("exiting");
    stop_servers(&app);
    g_main_loop_unref(app.loop);
    gst_deinit();
    return app.exit_code;
}
