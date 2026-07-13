#include "core/application.h"

#include <glib-unix.h>
#include <gst/gst.h>
#include <unistd.h>

#include <memory>

#include "control/control_server.h"
#include "control/handlers/register_handlers.h"
#include "pipeline/source_factory.h"
#include "v4l2/v4l2_factory.h"

Application::Application(std::string conf_path)
    : conf_path_(std::move(conf_path)),
      config_loader_(std::make_unique<FileConfigLoader>(conf_path_)),
      v4l2_factory_(create_v4l2_device_factory()),
      source_factory_(std::make_unique<SourceFactory>(*v4l2_factory_)) {
    register_all_handlers(registry_);
}

bool Application::start_servers() {
    rtsp_ = std::make_unique<RtspServer>(config_, *source_factory_);
    rtsp_->set_stall_handler([]() {
        // Hard exit, no orderly teardown: dismantling GStreamer around a
        // stalled pipeline crashed with SIGBUS on target, and systemd
        // restarts us either way -- a wedged process must not linger.
        _exit(1);
    });
    if (!rtsp_->start()) {
        rtsp_.reset();
        return false;
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
        if (!control_->start(rtsp_->bound_address(),
                             config_.control_port)) {
            control_.reset();
            rtsp_.reset();
            return false;
        }
    }

    if (config_.discovery_port > 0) {
        discovery_ = std::make_unique<DiscoveryServer>(config_);
        // Discovery is a convenience; a bind failure (port taken) is not
        // worth refusing to stream over.
        if (!discovery_->start())
            discovery_.reset();
    }

    if (config_.update_port > 0) {
        update_server_ = std::make_unique<UpdateServer>(swupdate_);
        if (!update_server_->start(rtsp_->bound_address(),
                                   config_.update_port))
            update_server_.reset();  // non-fatal: streaming still works
    }
    return true;
}

void Application::stop_servers() {
    update_server_.reset();
    discovery_.reset();
    control_.reset();  // before rtsp: its context reaches into the Application
    rtsp_.reset();
}

bool Application::start() {
    config_ = config_loader_->load();
    return start_servers();
}

void Application::reload() {
    g_message("reload: re-reading %s", conf_path_.c_str());

    Config previous = config_;
    config_ = config_loader_->load();

    stop_servers();  // release the ports before rebinding
    if (!start_servers()) {
        g_printerr("reload: new config failed, reverting\n");
        config_ = previous;
        stop_servers();
        if (!start_servers()) {
            g_printerr("reload: revert failed too, exiting\n");
            exit_code_ = 1;
            g_main_loop_quit(loop_);  // systemd restarts us
        }
    }
}

int Application::run() {
    loop_ = g_main_loop_new(nullptr, FALSE);
    g_unix_signal_add(SIGINT, on_signal, this);
    g_unix_signal_add(SIGTERM, on_signal, this);
    g_unix_signal_add(SIGHUP, on_reload, this);

    g_main_loop_run(loop_);

    g_message("exiting");
    stop_servers();
    g_main_loop_unref(loop_);
    return exit_code_;
}

gboolean Application::on_signal(gpointer user_data) {
    auto* self = static_cast<Application*>(user_data);
    g_message("shutdown signal received");
    g_main_loop_quit(self->loop_);
    return G_SOURCE_REMOVE;
}

gboolean Application::on_reload(gpointer user_data) {
    static_cast<Application*>(user_data)->reload();
    return G_SOURCE_CONTINUE;
}
