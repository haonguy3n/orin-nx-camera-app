#include "camera/core/Application.h"

#include <glib-unix.h>
#include <gst/gst.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include <condition_variable>
#include <functional>
#include <memory>
#include <utility>
#include <mutex>

#include "camera/control/ControlServer.h"
#include "camera/control/handlers/RegisterHandlers.h"
#include "camera/lib/net/NetworkResolver.h"
#include "camera/pipeline/SourceFactory.h"
#ifdef ENABLE_SECURE_USB
#include "camera/core/UsbAwareStreamController.h"
#include "camera/media/CameraPipeline.h"
#include "camera/secure/FfsGadget.h"
#endif

#include "camera/base/logging/xlog.h"

namespace camera {
namespace {

#ifdef ENABLE_SECURE_USB
// Detection is on whenever the model file is present (readable). No config
// toggle: shipping the model (CAMERA_FACE_MODEL) is the switch.
bool face_detection_available(const Config& cfg) {
    return !cfg.detect_model.empty()
           && access(cfg.detect_model.c_str(), R_OK) == 0;
}

// Detector working height for a camera: the configured width scaled to the
// camera's own aspect ratio, rounded even.
//
// It is NOT square. Scaling a 4:3 sensor (1440x1080) into a 320x320 detector
// input squashes every face horizontally by 33%, and YuNet -- trained on
// undistorted faces -- then misses most of them. This was the reason face
// detection "ran" but found nothing.
int detect_height_for(const CameraConfig& cam, int detect_width) {
    if (cam.width <= 0 || cam.height <= 0) return detect_width;
    const int height =
        static_cast<int>(static_cast<long long>(detect_width) * cam.height /
                         cam.width);
    const int even = height & ~1;
    return even < 2 ? 2 : even;
}

// What the secure USB transport should build for one camera: the source's
// capture fragment plus typed encode/detect parameters. The tee and both
// branches are constructed as objects in media::CameraPipeline -- the old
// find-and-replace of the RTP tail inside the launch string is gone, and with
// it the class of bug where a mismatched tail silently dropped a branch.
media::PipelineSpec usb_pipeline_spec(const CameraConfig& cam,
                                      const Config& cfg) {
    media::PipelineSpec spec;
    auto source = create_source(cam.source);
    if (!source)
        return spec;
    spec.source = source->build_source_fragment(cam);
    spec.h265 = cam.codec == "h265";
    spec.bitrate = cam.bitrate;
    // The test source has no NVIDIA hardware behind it (dev hosts, CI).
    spec.hw = cam.source != "test";
    // With a detection model present, the pipeline grows the raw branch the
    // detector consumes.
    if (face_detection_available(cfg)) {
        spec.detect_width = cfg.detect_width;
        spec.detect_height = detect_height_for(cam, cfg.detect_width);
    }
    return spec;
}
#endif  // ENABLE_SECURE_USB

#ifdef ENABLE_SECURE_USB
// Runs `work` on the GLib main loop and waits for it.
//
// Control handlers mutate the config and poke live GStreamer elements, so they
// must not run on a secure-USB session thread. The old loopback socket gave
// that for free (GSocket callbacks land on the main loop); dispatching
// in-process has to arrange it deliberately, or handlers race the video
// threads.
std::string run_on_main_loop(const std::function<std::string()>& work) {
    struct Job {
        const std::function<std::string()>* work;
        std::string result;
        std::mutex mutex;
        std::condition_variable done;
        bool finished = false;
    } job{&work, {}, {}, {}, false};

    g_main_context_invoke_full(
        nullptr, G_PRIORITY_DEFAULT,
        [](gpointer data) -> gboolean {
            auto* j = static_cast<Job*>(data);
            std::string out = (*j->work)();
            {
                std::lock_guard<std::mutex> lock(j->mutex);
                j->result = std::move(out);
                j->finished = true;
            }
            j->done.notify_one();
            return G_SOURCE_REMOVE;
        },
        &job, nullptr);

    std::unique_lock<std::mutex> lock(job.mutex);
    job.done.wait(lock, [&job] { return job.finished; });
    return std::move(job.result);
}
#endif  // ENABLE_SECURE_USB

// Network-mode delivery for detection boxes: the control connection, since
// there is no Meta channel without a secure session. Same JSON payload the USB
// path sends, so the host parses one format either way.
class ControlMetaSink : public detect::IMetaSink {
public:
    explicit ControlMetaSink(ControlServer** server) : server_(server) {}

    void on_meta(uint8_t camera, const std::string& json) override {
        ControlServer* server = server_ != nullptr ? *server_ : nullptr;
        if (server == nullptr) return;
        // Called from a detection thread; broadcast touches the connection set
        // owned by the main loop, so hop over. Fire-and-forget: boxes are
        // droppable and must never hold up the detector.
        auto* line = new std::string("{\"event\":\"faces\",\"camera\":" +
                                     std::to_string(camera) + ",\"data\":" +
                                     json + "}");
        auto* target = server;
        g_main_context_invoke_full(
            nullptr, G_PRIORITY_DEFAULT_IDLE,
            [](gpointer data) -> gboolean {
                auto* payload = static_cast<std::pair<ControlServer*,
                                                      std::string*>*>(data);
                payload->first->broadcast(*payload->second);
                delete payload->second;
                delete payload;
                return G_SOURCE_REMOVE;
            },
            new std::pair<ControlServer*, std::string*>(target, line), nullptr);
    }

private:
    ControlServer** server_;
};

}  // namespace

Application::Application(std::string conf_path)
    : conf_path_(std::move(conf_path)),
      config_loader_(conf_path_) {
    register_all_handlers(registry_);
}

camera::base::Expected<camera::base::Unit, std::string> Application::start_servers() {
    // transports=usb: the secure USB transport owns the cameras and taps the
    // encoder directly, control is dispatched in-process and update rides an
    // anonymous socketpair. Nothing needs a TCP socket, so nothing is created:
    // no RTSP server, no control listener, no loopback update listener. The
    // only listener left is the NCM recovery channel below, which is
    // deliberate -- it is how firmware gets pushed when the USB transport
    // itself is broken.
    const bool usb_only = config_.transports == "usb";
    if (!usb_only) {
        rtsp_ = std::make_unique<RtspServer>(config_);
        // Network mode: detection boxes go out over the control connection.
        // The sink reads control_ through a pointer because the control server
        // is constructed after the mounts are built.
        meta_sink_ = std::make_unique<ControlMetaSink>(&control_ptr_);
        rtsp_->set_meta_sink(meta_sink_.get());
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
    } else {
        XLOGF(INFO, "transports=usb: served over USB only -- no RTSP, control "
                    "in-process, update over a socketpair; no TCP listener "
                    "except the recovery channel");
    }

    // Runtime settings must reach whichever pipeline is actually serving. With
    // transports=usb the RTSP pipeline is never instantiated, so a plain
    // RtspServer controller made set-exposure/set-gain/set-zoom no-ops.
#ifdef ENABLE_SECURE_USB
    stream_controller_ = std::make_unique<UsbAwareStreamController>(
        rtsp_.get(), [this] { return secure_usb_.get(); },
        [this](int cam) {
            return cam >= 0 && cam < Config::kNumCameras
                       ? usb_pipeline_spec(config_.cameras[cam], config_)
                       : media::PipelineSpec{};
        });
#endif
    IStreamController& stream_for_handlers =
        stream_controller_ ? *stream_controller_
                           : static_cast<IStreamController&>(*rtsp_);
    // Without the secure transport compiled in there is no controller, and
    // usb mode would leave nothing to drive -- that combination is refused at
    // load rather than crashing on the first control call.
    if (!stream_controller_ && rtsp_ == nullptr)
        return camera::base::makeUnexpected(
            std::string("transports=usb requires the secure USB transport"));

    control_context_ = std::make_unique<ControlContext>(ControlContext{
            config_,
            stream_for_handlers,
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
    });

    // In usb mode control arrives on the USB endpoint and is dispatched
    // in-process, so no listener is created at all -- previously it bound
    // 127.0.0.1:8555 purely so the transport could talk to itself.
    if (config_.control_port > 0 && !usb_only) {
        control_ = std::make_unique<ControlServer>(registry_,
                                                    *control_context_);
        control_ptr_ = control_.get();
        if (auto r = control_->start(rtsp_->bound_address(),
                                     config_.control_port);
            !r) {
            control_.reset();
            rtsp_.reset();
            return r;
        }
    }

    // Discovery answers broadcasts so a host can find the device on the
    // network. Over USB the host already has it, so in usb mode this would be
    // the one remaining thing listening on 0.0.0.0 for no reason.
    if (config_.discovery_port > 0 && !usb_only) {
        discovery_ = std::make_unique<DiscoveryServer>(config_);
        // Discovery is a convenience; a bind failure (port taken) is not
        // worth refusing to stream over.
        if (auto r = discovery_->start(); !r) {
            XLOGF(WARN, "%s", r.error().c_str());
            discovery_.reset();
        }
    }

    if (config_.update_port > 0) {
        // The object is always needed -- the secure transport hands it a
        // socketpair via adopt_fd() -- but in usb mode it is never bound.
        // In network mode this binds normally. In usb mode it is left unbound:
        // Update records are handed to it over an anonymous socketpair, so
        // there is nothing for a listener to accept.
        update_server_ = std::make_unique<UpdateServer>(swupdate_, config_);
        if (!usb_only) {
            if (auto r = update_server_->start(rtsp_->bound_address(),
                                               config_.update_port);
                !r) {
                XLOGF(WARN, "%s", r.error().c_str());
                update_server_.reset();  // non-fatal: streaming still works
            }
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
        // Hand the gadget back so NCM/ACM can bind. Without this a switch from
        // usb to network left ffs.secure in the config with no descriptors,
        // configfs refused to bind, and USB networking never came up -- the
        // device served RTSP onto a link the host could not reach.
        secure::FfsGadget::release_base_gadget();
    } else {
        // Secure USB is additive. Failure to expose the optional FunctionFS
        // interface must never take down the established NCM RTSP path.
        // Direct encoder tap only when the RTSP server is not also driving
        // the sensors: Argus permits a single consumer per camera.
        std::vector<media::PipelineSpec> video_launch;
        if (usb_only) {
            for (int i = 0; i < Config::kNumCameras; ++i) {
                video_launch.push_back(
                    config_.cameras[i].enabled
                        ? usb_pipeline_spec(config_.cameras[i], config_)
                        : media::PipelineSpec{});
            }
        }
        secure_usb_ = std::make_unique<secure::SecureUsbServer>(
            config_.tls_cert.empty() ? "/etc/camera-streamer/tls/server.crt"
                                     : config_.tls_cert,
            config_.tls_key.empty() ? "/etc/camera-streamer/tls/server.key"
                                    : config_.tls_key,
            std::move(video_launch));
        if (usb_only && face_detection_available(config_)) {
            // Aspect-correct working size, matching the detect branch caps so
            // the detector never rescales (and never re-distorts) the frame.
            const int detect_h =
                detect_height_for(config_.cameras[0], config_.detect_width);
            secure_usb_->set_face_detection(config_.detect_model,
                                            config_.detect_width, detect_h,
                                            config_.detect_score,
                                            config_.detect_fps);
            XLOGF(INFO,
                  "face detection enabled: %s (%dx%d, score>=%.2f, %d fps)",
                  config_.detect_model.c_str(), config_.detect_width,
                  detect_h, config_.detect_score, config_.detect_fps);
        } else if (usb_only && !config_.detect_model.empty()) {
            XLOGF(INFO, "face detection off: model not found at %s",
                  config_.detect_model.c_str());
        }
        // Control in-process: no loopback socket for Channel::Control. The
        // context must be the same one the TCP server uses, so both paths see
        // identical state; control_context_ owns it for the process lifetime.
        if (control_context_) {
            ControlContext* ctx = control_context_.get();
            ControlRegistry* registry = &registry_;
            secure_usb_->set_control_dispatcher(
                [ctx, registry](const std::string& line) {
                    return run_on_main_loop([ctx, registry, &line] {
                        return dispatch_request(*registry, *ctx, line.c_str());
                    });
                });
        }
        // Update in-process: an anonymous socketpair replaces the TCP
        // connection to 127.0.0.1:8557. The far end is adopted by the update
        // server and runs its normal handler, so upload behaviour (including
        // close() meaning end-of-upload) is unchanged -- only the transport
        // underneath it is, which is the point: nothing on the network stack.
        if (update_server_) {
            UpdateServer* server = update_server_.get();
            secure_usb_->set_update_channel_factory([server]() -> int {
                int pair[2] = {-1, -1};
                if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) != 0) {
                    XLOGF(WARN, "update: socketpair failed: %s", strerror(errno));
                    return -1;
                }
                if (!server->adopt_fd(pair[0])) {  // takes ownership of pair[0]
                    close(pair[1]);
                    return -1;
                }
                return pair[1];  // tunnel writes the upload here
            });
        }
        std::string secure_error;
        if (!secure_usb_->start(&secure_error)) {
            XLOGF(WARN, "secure-usb disabled: %s", secure_error.c_str());
            secure_usb_.reset();
            // Same reason as the network-mode path: if we are not going to own
            // FunctionFS, give the gadget back so NCM/ACM binds and the device
            // stays reachable -- including over the recovery channel, which is
            // exactly what is needed when secure USB has just failed.
            secure::FfsGadget::release_base_gadget();
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
    config_ = config_loader_.load();
    return start_servers();
}

void Application::reload() {
    XLOGF(INFO, "reload: re-reading %s", conf_path_.c_str());

    Config previous = config_;
    config_ = config_loader_.load();

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
