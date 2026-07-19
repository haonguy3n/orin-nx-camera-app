#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace camera::secure {

// Owns the FunctionFS vendor interface in the camera-streamer process.  It is
// deliberately additive: RTSP/control over CDC-NCM continue to be served by
// the normal application sockets.
class SecureUsbServer {
public:
    // `video_launch` holds one gst_parse_launch description per camera, each
    // ending in `appsink name=sink`, so frames are taken straight from the
    // encoder. Empty entries fall back to pulling the camera's local RTSP
    // mount, which is required when the RTSP server also owns the sensor --
    // Argus permits only one consumer per camera.
    SecureUsbServer(std::string certificate, std::string private_key,
                    std::vector<std::string> video_launch = {});
    ~SecureUsbServer();
    bool start(std::string* error);
    void stop();

    // Enable on-device face detection: each camera's pipeline grows a raw
    // branch and a detection thread that emits boxes over Channel::Meta.
    // `model` is the YuNet .onnx path; empty (default) leaves detection off.
    // Must be called before start().
    void set_face_detection(std::string model, int input_width, int input_height,
                            double score_threshold, int detect_fps);
    // set-stream: starts/stops one camera's video push without touching the
    // session. Off = the video loop parks and its pipeline (and sensor) is
    // released; on = it respawns within ~200 ms.
    void set_stream_enabled(uint8_t camera, bool enabled);

    // Dispatches one control request line and returns the reply line. Set by
    // the application; when present, Channel::Control records are handled
    // IN-PROCESS instead of being written to a loopback TCP server. The
    // implementation is responsible for running on the GLib main loop.
    using ControlDispatcher = std::function<std::string(const std::string&)>;
    void set_control_dispatcher(ControlDispatcher dispatcher);

    // Opens an in-process channel to the update server and returns the
    // writable end, or -1. Set by the application; when present, Update
    // records go through an anonymous socketpair instead of a TCP connection
    // to 127.0.0.1:8557 -- same fd semantics (close = end of upload), nothing
    // on the network stack.
    using UpdateChannelFactory = std::function<int()>;
    void set_update_channel_factory(UpdateChannelFactory factory);

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

    // Replace the launch description and ask the running pipeline to rebuild
    // with it. Needed for anything the source cannot change live (zoom is a
    // crop on nvvidconv), and so later sessions do not revert: the old
    // description was frozen at construction.
    void refresh_launch(uint8_t camera, std::string launch);

private:
    void run();
    // Serves one authenticated session, returning when it dies. Blocks, so it
    // owns its own threads: FunctionFS endpoint files implement no .poll, and
    // poll() on them reports ready unconditionally, which is why the earlier
    // single-threaded event loop could not work.
    void serve_session(int ep0);
    // Direct encoder tap when configured, otherwise the camera's local RTSP
    // mount.
    std::string video_description(uint8_t camera) const;
    // Publishes/clears the live source for set_source_property.
    void publish_source(uint8_t camera, void* element);
    // Called per frame by the video loops.
    void report_frame(uint8_t camera, bool streaming, size_t bytes);

    std::string certificate_;
    std::string private_key_;
    std::vector<std::string> video_launch_;
    ControlDispatcher control_dispatcher_;
    UpdateChannelFactory update_channel_factory_;
    std::string detect_model_;
    int detect_width_ = 320;
    double detect_score_ = 0.45;
    int detect_fps_ = 10;
    int detect_height_ = 320;
    std::atomic<bool> stream_enabled_[2] = {{true}, {true}};
    // Live source element per camera, published by the video loop while its
    // pipeline is up. Guarded because the control server pokes it from the
    // GLib main thread while the video loop replaces it.
    mutable std::mutex live_mutex_;
    void* live_source_[2] = {nullptr, nullptr};  // GstElement*, ref held
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
