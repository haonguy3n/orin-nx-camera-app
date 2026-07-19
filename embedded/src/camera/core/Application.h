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

#include "camera/config/Config.h"
#include "camera/config/ConfigLoader.h"
#include "camera/core/Watchdog.h"
#include "camera/control/ControlContext.h"
#include "camera/control/ControlRegistry.h"
#include "camera/control/ControlServer.h"
#include "camera/detect/MetaSink.h"
#include "camera/discovery/DiscoveryServer.h"
#include "camera/rtsp/RtspServer.h"
#include "camera/core/StreamController.h"
#include "camera/update/SwupdateClient.h"
#include "camera/update/UpdateServer.h"
#ifdef ENABLE_SECURE_USB
#include "camera/secure/SecureUsbServer.h"
#endif
#include "camera/base/Expected.h"
#include "camera/base/Unit.h"
#include "camera/base/io/async/EventBase.h"

namespace camera {

class Application {
public:
    explicit Application(std::string conf_path);

    // Loads config and starts all servers. Returns the failure reason on
    // error (main logs it; systemd Restart=on-failure retries).
    camera::base::Expected<camera::base::Unit, std::string> start();

    // Runs the GMainLoop. Returns the process exit code.
    int run();

    // Triggers a config reload (SIGHUP or control protocol "reload").
    void reload();

private:
    camera::base::Expected<camera::base::Unit, std::string> start_servers();
    void stop_servers();

    static gboolean on_signal(gpointer user_data);
    static gboolean on_reload(gpointer user_data);

    std::string conf_path_;
    FileConfigLoader config_loader_;
    Config config_;

    // Infrastructure (created once, live across reloads).
    ControlRegistry registry_;
    SwupdateClient swupdate_;  // live across reloads (update may span reloads)

    // Servers (recreated on reload).
    std::unique_ptr<RtspServer> rtsp_;
    // Fans runtime settings to the USB pipeline as well as RTSP; must
    // outlive control_, which holds a reference to it.
    std::unique_ptr<IStreamController> stream_controller_;
    // Owned so the secure-USB in-process dispatcher can use the same
    // context as the TCP control server (it holds references, so both
    // see identical state).
    std::unique_ptr<ControlContext> control_context_;
    std::unique_ptr<ControlServer> control_;
    // Raw pointer handed to the meta sink, which outlives nothing but
    // needs to see control_ once it exists.
    ControlServer* control_ptr_ = nullptr;
    std::unique_ptr<detect::IMetaSink> meta_sink_;
    std::unique_ptr<DiscoveryServer> discovery_;
    std::unique_ptr<UpdateServer> update_server_;
    // transports=usb only: second instance on the CDC-NCM address, so
    // firmware can still be pushed when the secure transport is broken.
    std::unique_ptr<UpdateServer> update_recovery_;
#ifdef ENABLE_SECURE_USB
    std::unique_ptr<secure::SecureUsbServer> secure_usb_;
#endif

    Watchdog watchdog_;
    camera::base::EventBase evb_;
    int exit_code_ = 0;
};

}  // namespace camera
