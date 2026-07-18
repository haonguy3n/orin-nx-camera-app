#pragma once

#include <atomic>
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
    // set-stream: starts/stops one camera's video push without touching the
    // session. Off = the video loop parks and its pipeline (and sensor) is
    // released; on = it respawns within ~200 ms.
    void set_stream_enabled(uint8_t camera, bool enabled);

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

    std::string certificate_;
    std::string private_key_;
    std::vector<std::string> video_launch_;
    std::atomic<bool> stream_enabled_[2] = {{true}, {true}};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> worker_exited_{false};
    std::thread worker_;
};

}  // namespace camera::secure
