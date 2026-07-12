// RTSP server exposing one mount per enabled camera (/cam0, /cam1).
#pragma once

#include <gst/rtsp-server/rtsp-server.h>

#include <atomic>
#include <functional>
#include <string>

#include "config.h"

// Runtime state of one mount, for get-status and the watchdog.
struct StreamStatus {
    bool mounted = false;    // camera enabled, factory installed
    bool streaming = false;  // a (shared) media pipeline is currently live
    guint64 frames = 0;      // buffers through the payloader since start()
};

class RtspServer {
public:
    explicit RtspServer(const Config& config);
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    // Creates the media factories and attaches the server to the default
    // GMainContext. Returns false if no camera is enabled or attach fails.
    bool start();

    const std::string& bound_address() const { return address_; }
    int client_count() const { return clients_; }
    StreamStatus stream_status(int cam);

    // Sets a property on camera |cam|'s live source element (argus runtime
    // exposure/gain — nvarguscamerasrc forwards range properties while
    // streaming). |value| is converted per gst_util_set_object_arg. Returns
    // false if no pipeline is live; the caller keeps the config updated so
    // later pipelines pick the value up from the launch string.
    bool set_source_property(int cam, const char* property, const char* value);

    // Called (from the main loop) when a live pipeline stopped producing
    // frames; the service exits so systemd restarts it cleanly.
    void set_stall_handler(std::function<void()> handler) {
        stall_handler_ = std::move(handler);
    }

private:
    struct CamState {
        RtspServer* self = nullptr;
        int index = 0;
        bool mounted = false;
        GWeakRef media;   // live GstRTSPMedia (shared factory: one at a time)
        GWeakRef source;  // "camsrc" element inside the live pipeline
        std::atomic<guint64> frames{0};
        // Watchdog bookkeeping (main loop thread only).
        guint64 last_frames = 0;
        int stalled_checks = 0;
    };

    static void on_media_configure(GstRTSPMediaFactory* factory,
                                   GstRTSPMedia* media, gpointer user_data);
    static void on_media_unprepared(GstRTSPMedia* media, gpointer user_data);
    static gboolean on_watchdog(gpointer user_data);

    const Config& config_;
    std::string address_;
    GstRTSPServer* server_ = nullptr;
    guint attach_id_ = 0;
    guint watchdog_id_ = 0;
    int clients_ = 0;
    CamState cams_[Config::kNumCameras];
    std::function<void()> stall_handler_;
};
