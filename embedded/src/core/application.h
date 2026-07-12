// Application: owns everything with process lifetime.
//
// Replaces the original main.cpp's App struct + free functions with a
// proper class. Manages config loading, server lifecycle (start/stop/
// reload), and signal handling. The control server's ControlContext
// references this class's members, which are replaced on reload.
#pragma once

#include <glib.h>
#include <gst/gst.h>

#include <memory>
#include <string>

#include "config/config.h"
#include "config/config_loader.h"
#include "control/control_context.h"
#include "control/control_registry.h"
#include "control/control_server.h"
#include "discovery/discovery_server.h"
#include "pipeline/source_factory.h"
#include "rtsp/rtsp_server.h"
#include "v4l2/v4l2_device.h"

class Application {
public:
    explicit Application(std::string conf_path);

    // Loads config and starts all servers. Returns false on failure
    // (systemd Restart=on-failure retries).
    bool start();

    // Runs the GMainLoop. Returns the process exit code.
    int run();

    // Triggers a config reload (SIGHUP or control protocol "reload").
    void reload();

private:
    bool start_servers();
    void stop_servers();

    static gboolean on_signal(gpointer user_data);
    static gboolean on_reload(gpointer user_data);

    std::string conf_path_;
    std::unique_ptr<IConfigLoader> config_loader_;
    Config config_;

    // Infrastructure (created once, live across reloads).
    std::unique_ptr<IV4l2DeviceFactory> v4l2_factory_;
    std::unique_ptr<ISourceFactory> source_factory_;
    ControlRegistry registry_;

    // Servers (recreated on reload).
    std::unique_ptr<RtspServer> rtsp_;
    std::unique_ptr<ControlServer> control_;
    std::unique_ptr<DiscoveryServer> discovery_;

    GMainLoop* loop_ = nullptr;
    int exit_code_ = 0;
};
