#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "camera/media/CameraPipeline.h"
#include "camera/base/Expected.h"
#include "camera/base/Unit.h"
#include "camera/secure/FfsGadget.h"
#include "secure/SecureUsbContext.h"

namespace camera::secure {

// Owns the FunctionFS vendor interface in the camera-streamer process. In USB
// mode it carries video/control/update while CDC-NCM remains only for recovery.
class SecureUsbServer {
public:
    using ControlDispatcher = std::function<std::string(const std::string&)>;
    using UpdateChannelFactory = std::function<int()>;

    struct FaceDetection {
        std::string model;
        double score_threshold = 0.45;
        int fps = 10;
    };

    struct Options {
        std::string certificate;
        std::string private_key;
        // One typed pipeline per camera; an empty spec disables that camera.
        std::vector<media::PipelineSpec> video;
        std::optional<FaceDetection> face_detection;
        ControlDispatcher control_dispatcher;
        UpdateChannelFactory update_channel_factory;
    };

    // Validates credentials and detection settings and publishes FunctionFS.
    // Like SSLContext::create(), success means initialization is complete and
    // the object is safe to use; there is no order-dependent setter phase.
    [[nodiscard]] static base::Expected<std::unique_ptr<SecureUsbServer>, std::string>
    create(Options options);

    ~SecureUsbServer();
    base::Expected<base::Unit, std::string> start();
    void stop();
    // set-stream: starts/stops one camera's video push without touching the
    // session. Off = the video loop parks and its pipeline (and sensor) is
    // released; on = it respawns within ~200 ms.
    void set_stream_enabled(uint8_t camera, bool enabled);

    // Live per-camera state, so get-status can report the transport that is
    // actually serving. Without this, usb mode reports an empty status: the
    // only IStreamController was RtspServer, and usb mode no longer builds one.
    struct CameraStats {
        bool streaming = false;
        uint64_t frames = 0;
        uint64_t bytes = 0;
        double fps = 0.0;
    };
    [[nodiscard]] CameraStats stats(uint8_t camera) const;

    // Runtime control of the USB pipeline, mirroring what IStreamController
    // does for the RTSP mounts. Without these, set-exposure/set-gain/set-zoom
    // reached only the RTSP pipeline -- which transports=usb never even
    // instantiates -- so they silently did nothing.

    // Poke a property on the live nvarguscamerasrc (argus forwards its range
    // properties while streaming). False when no pipeline is up for `camera`.
    bool set_source_property(uint8_t camera, const char* property,
                             const char* value);

    // Replace the pipeline spec and ask the running pipeline to rebuild
    // with it. Needed for anything the source cannot change live (zoom is a
    // crop on nvvidconv), and so later sessions do not revert: the old
    // description was frozen at construction.
    void refresh_launch(uint8_t camera, media::PipelineSpec launch);

private:
    SecureUsbServer(Options options, SecureUsbContext context,
                    std::unique_ptr<FfsGadget> gadget);
    void run();
    // Serves one authenticated session, returning when it dies. Blocks, so it
    // owns its own threads: FunctionFS endpoint files implement no .poll, and
    // poll() on them reports ready unconditionally, which is why the earlier
    // single-threaded event loop could not work.
    void serve_session(int ep0);
    // Direct encoder tap; an empty spec means the camera is not served.
    media::PipelineSpec video_spec(uint8_t camera) const;
    // Publishes/clears the live source for set_source_property.
    void publish_source(uint8_t camera, void* element);
    // Called per frame by the video loops.
    void report_frame(uint8_t camera, bool streaming, size_t bytes);

    SecureUsbContext context_;
    std::unique_ptr<FfsGadget> gadget_;
    std::vector<media::PipelineSpec> video_launch_;
    ControlDispatcher control_dispatcher_;
    UpdateChannelFactory update_channel_factory_;
    std::string detect_model_;
    double detect_score_ = 0.45;
    int detect_fps_ = 10;
    std::atomic<bool> stream_enabled_[2] = {{true}, {true}};
    // Live source element per camera, published by the video loop while its
    // pipeline is up. Guarded because the control server pokes it from the
    // GLib main thread while the video loop replaces it.
    mutable std::mutex live_mutex_;
    // media::CameraPipeline* for each camera while its loop runs. Not
    // refcounted: the loop publishes nullptr before the pipeline dies,
    // under live_mutex_.
    void* live_source_[2] = {nullptr, nullptr};
    std::atomic<bool> relaunch_[2] = {{false}, {false}};
    // Updated by the video loops, read by the control server.
    std::atomic<bool> streaming_[2] = {{false}, {false}};
    std::atomic<uint64_t> frames_[2] = {{0}, {0}};
    std::atomic<uint64_t> bytes_[2] = {{0}, {0}};
    std::atomic<uint64_t> fps_milli_[2] = {{0}, {0}};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> worker_exited_{false};
    std::thread worker_;
};

}  // namespace camera::secure
