#include "camera/core/Application.h"

#include <glib-unix.h>
#include <gst/gst.h>
#include <unistd.h>

#include <memory>

#include "camera/control/ControlServer.h"
#include "camera/control/handlers/RegisterHandlers.h"
#include "camera/lib/net/NetworkResolver.h"
#include "camera/pipeline/PipelineBuilder.h"
#include "camera/pipeline/SourceFactory.h"
#include "camera/lib/v4l2/V4l2Factory.h"

#include "camera/base/logging/xlog.h"

namespace camera {
namespace {

// The encode chain a source would use for RTSP, terminated at an appsink so
// the secure USB transport gets the elementary stream straight from the
// encoder.
// Detection is on whenever the model file is present (readable). No config
// toggle: shipping the model (CAMERA_FACE_MODEL) is the switch.
bool face_detection_available(const Config& cfg) {
    return !cfg.detect_model.empty()
           && access(cfg.detect_model.c_str(), R_OK) == 0;
}

std::string usb_video_launch(ISourceFactory& factory, const CameraConfig& cam,
                             const Config& cfg) {
    auto source = factory.create(cam.source);
    if (!source)
        return {};
    std::string launch = source->build_launch(cam);
    // build_launch ends with the RTP payloader inside "( ... )"; swap that
    // tail for the appsink one rather than duplicating the source setup.
    const std::string rtp_tail = PipelineBuilder::nvenc_tail(cam);
    const size_t at = launch.rfind(rtp_tail);
    if (at == std::string::npos)
        return {};
    // With a detection model present, tee the source: one leg encodes as
    // before, the other is the raw branch the detector consumes.
    std::string tail = PipelineBuilder::appsink_tail(cam);
    if (face_detection_available(cfg)) {
        tail = "tee name=t  t. ! " + tail + "  t. ! "
             + PipelineBuilder::detect_branch(cfg.detect_width, cfg.detect_height);
    }
    launch.replace(at, rtp_tail.size(), tail);
    return launch;
}

}  // namespace

Application::Application(std::string conf_path)
    : conf_path_(std::move(conf_path)),
      config_loader_(std::make_unique<FileConfigLoader>(conf_path_)),
      v4l2_factory_(create_v4l2_device_factory()),
      source_factory_(std::make_unique<SourceFactory>(*v4l2_factory_)) {
    register_all_handlers(registry_);
}

camera::base::Expected<camera::base::Unit, std::string> Application::start_servers() {
    // transports=usb: the cameras are owned by the secure USB transport,
    // which taps the encoder directly. No RTSP server, so nothing is
    // payloaded to RTP and bounced through loopback just to be taken apart
    // again. Control and update still bind, but only on 127.0.0.1, since the
    // USB transport reaches them there.
    const bool usb_only = config_.transports == "usb";
    if (usb_only && config_.listen != "127.0.0.1") {
        XLOGF(INFO, "transports=usb: cameras served over USB only, "
                    "control/update on loopback");
        config_.listen = "127.0.0.1";
    }
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
            [this](int camera, bool enabled) {
#ifdef ENABLE_SECURE_USB
                if (secure_usb_)
                    secure_usb_->set_stream_enabled(static_cast<uint8_t>(camera),
                                                    enabled);
#else
                (void)camera;
                (void)enabled;
#endif
            },
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
        // The primary update listener stays on the servers' bound address --
        // loopback under transports=usb, which is where the secure USB
        // tunnel delivers Update-channel records. (An earlier version MOVED
        // this to the NCM address in usb mode, which silently disconnected
        // the tunnel's update path: its forwards target 127.0.0.1:8557.)
        update_server_ = std::make_unique<UpdateServer>(swupdate_, config_);
        if (auto r = update_server_->start(rtsp_->bound_address(),
                                           config_.update_port);
            !r) {
            XLOGF(WARN, "%s", r.error().c_str());
            update_server_.reset();  // non-fatal: streaming still works
        }
        // Recovery path, in ADDITION: with transports=usb everything else is
        // on loopback, so a secure transport that fails to come up would
        // leave no way to push firmware. A second listener on the CDC-NCM
        // gadget address rides the same physical cable and shares no code
        // with the secure path.
        if (usb_only && config_.recovery_update) {
            const std::string ncm = NetworkResolver::resolve_listen("usb");
            if (!ncm.empty()) {
                update_recovery_ = std::make_unique<UpdateServer>(swupdate_, config_);
                if (auto r = update_recovery_->start(ncm, config_.update_port); !r) {
                    XLOGF(WARN, "update recovery channel: %s", r.error().c_str());
                    update_recovery_.reset();
                } else {
                    XLOGF(INFO, "update: recovery channel on %s:%d",
                          ncm.c_str(), config_.update_port);
                }
            } else {
                XLOGF(WARN, "update: no CDC-NCM address for the recovery channel");
            }
        }
    }
#ifdef ENABLE_SECURE_USB
    if (config_.transports == "network") {
        XLOGF(INFO, "transports=network: secure USB endpoint not published");
    } else {
        // Secure USB is additive. Failure to expose the optional FunctionFS
        // interface must never take down the established NCM RTSP path.
        // Direct encoder tap only when the RTSP server is not also driving
        // the sensors: Argus permits a single consumer per camera.
        std::vector<std::string> video_launch;
        if (usb_only) {
            for (int i = 0; i < Config::kNumCameras; ++i) {
                video_launch.push_back(
                    config_.cameras[i].enabled
                        ? usb_video_launch(*source_factory_, config_.cameras[i],
                                           config_)
                        : std::string());
            }
        }
        secure_usb_ = std::make_unique<secure::SecureUsbServer>(
            config_.tls_cert.empty() ? "/etc/camera-streamer/tls/server.crt"
                                     : config_.tls_cert,
            config_.tls_key.empty() ? "/etc/camera-streamer/tls/server.key"
                                    : config_.tls_key,
            std::move(video_launch));
        if (usb_only && face_detection_available(config_)) {
            secure_usb_->set_face_detection(config_.detect_model,
                                            config_.detect_width,
                                            config_.detect_height);
            XLOGF(INFO, "face detection enabled: %s (%dx%d)",
                  config_.detect_model.c_str(), config_.detect_width,
                  config_.detect_height);
        } else if (usb_only && !config_.detect_model.empty()) {
            XLOGF(INFO, "face detection off: model not found at %s",
                  config_.detect_model.c_str());
        }
        std::string secure_error;
        if (!secure_usb_->start(&secure_error)) {
            XLOGF(WARN, "secure-usb disabled: %s", secure_error.c_str());
            secure_usb_.reset();
        }
    }
#endif
    return camera::base::unit;
}

void Application::stop_servers() {
#ifdef ENABLE_SECURE_USB
    secure_usb_.reset();
#endif
    update_recovery_.reset();
    update_server_.reset();
    discovery_.reset();
    control_.reset();  // before rtsp: its context reaches into the Application
    rtsp_.reset();
}

camera::base::Expected<camera::base::Unit, std::string> Application::start() {
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
