#include "camera/core/Application.h"

#include <glib-unix.h>
#include <gst/gst.h>
#include <unistd.h>

#include <memory>

#include "camera/control/ControlServer.h"
#include "camera/control/handlers/RegisterHandlers.h"
#include "camera/pipeline/SourceFactory.h"
#include "camera/lib/v4l2/V4l2Factory.h"

#include "camera/folly/logging/xlog.h"

namespace camera {

Application::Application(std::string conf_path)
    : conf_path_(std::move(conf_path)),
      config_loader_(std::make_unique<FileConfigLoader>(conf_path_)),
      v4l2_factory_(create_v4l2_device_factory()),
      source_factory_(std::make_unique<SourceFactory>(*v4l2_factory_)) {
    register_all_handlers(registry_);
}

folly::Expected<folly::Unit, std::string> Application::start_servers() {
    rtsp_ = std::make_unique<RtspServer>(config_, *source_factory_);
    rtsp_->set_stall_handler([]() {
        // Every camera is dead -- nothing left to serve. Hard exit, no
        // orderly teardown: dismantling GStreamer around a stalled
        // pipeline crashed with SIGBUS on target, and systemd restarts
        // us either way -- a wedged process must not linger.
        _exit(1);
    });
    if (auto r = rtsp_->start(); !r) {
        rtsp_.reset();
        return r;
    }

    if (config_.control_port > 0) {
        ControlContext ctx{
            config_,
            *rtsp_,
            *v4l2_factory_,
            *source_factory_,
            swupdate_,
            [this]() { reload(); },
        };
        control_ = std::make_unique<ControlServer>(registry_,
                                                    std::move(ctx));
        if (auto r = control_->start(rtsp_->bound_address(),
                                     config_.control_port);
            !r) {
            control_.reset();
            rtsp_.reset();
            return r;
        }
    }

    if (config_.discovery_port > 0) {
        discovery_ = std::make_unique<DiscoveryServer>(config_);
        // Discovery is a convenience; a bind failure (port taken) is not
        // worth refusing to stream over.
        if (auto r = discovery_->start(); !r) {
            XLOGF(WARN, "%s", r.error().c_str());
            discovery_.reset();
        }
    }

    if (config_.update_port > 0) {
        update_server_ = std::make_unique<UpdateServer>(swupdate_, config_);
        if (auto r = update_server_->start(rtsp_->bound_address(),
                                           config_.update_port);
            !r) {
            XLOGF(WARN, "%s", r.error().c_str());
            update_server_.reset();  // non-fatal: streaming still works
        }
    }
    return folly::unit;
}

void Application::stop_servers() {
    update_server_.reset();
    discovery_.reset();
    control_.reset();  // before rtsp: its context reaches into the Application
    rtsp_.reset();
}

folly::Expected<folly::Unit, std::string> Application::start() {
    config_ = config_loader_->load();
    return start_servers();
}

void Application::reload() {
    XLOGF(INFO, "reload: re-reading %s", conf_path_.c_str());

    Config previous = config_;
    config_ = config_loader_->load();

    stop_servers();  // release the ports before rebinding
    if (auto r = start_servers(); !r) {
        XLOGF(ERR, "reload: new config failed (%s), reverting",
              r.error().c_str());
        config_ = previous;
        stop_servers();
        if (auto revert = start_servers(); !revert) {
            XLOGF(ERR, "reload: revert failed too (%s), exiting",
                  revert.error().c_str());
            exit_code_ = 1;
            evb_.terminateLoopSoon();  // systemd restarts us
        }
    }
}

int Application::run() {
    g_unix_signal_add(SIGINT, on_signal, this);
    g_unix_signal_add(SIGTERM, on_signal, this);
    g_unix_signal_add(SIGHUP, on_reload, this);

    watchdog_.start();  // READY=1 + periodic WATCHDOG=1 from this loop

    evb_.loopForever();

    watchdog_.stop();

    XLOGF(INFO, "exiting");
    stop_servers();
    return exit_code_;
}

gboolean Application::on_signal(gpointer user_data) {
    auto* self = static_cast<Application*>(user_data);
    XLOGF(INFO, "shutdown signal received");
    self->evb_.terminateLoopSoon();
    return G_SOURCE_REMOVE;
}

gboolean Application::on_reload(gpointer user_data) {
    static_cast<Application*>(user_data)->reload();
    return G_SOURCE_CONTINUE;
}

}  // namespace camera
