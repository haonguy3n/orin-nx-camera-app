// Per-camera RTSP mount controller.
//
// Manages one camera's RTSP mount: the media factory, weak refs to the
// live media and source element, and a frame counter on the payloader for
// get-status and the stall watchdog. This was the CamState struct +
// associated logic in the original rtsp_server.cpp, extracted into its
// own class for SRP (one class per camera mount, not one server managing
// all mounts' internal state).
#pragma once

#include <thread>
#include "camera/detect/MetaSink.h"

#include <gst/rtsp-server/rtsp-server.h>

#include <atomic>
#include <string>

#include "camera/core/StreamController.h"

namespace camera {

// Stall watchdog: a live PLAYING pipeline that pushes no buffer through its
// payloader for kStallChecks consecutive periods is declared dead.
constexpr int kWatchdogPeriodSec = 5;
constexpr int kStallChecks = 3;

class MountController {
public:
    // Runs face detection on this mount's "detect" appsink, when the launch
    // string carries one (see PipelineBuilder::rtsp_launch).
    // Detection is per-media: gst-rtsp-server builds the pipeline on client
    // connect, so this starts when a client arrives and stops when the media
    // goes away -- the same "no session, no detection" property the USB path
    // has. `meta` receives to_meta_json payloads from the detection thread.
    void enable_detection(detect::IMetaSink* meta, std::string model,
                          int width, int height, double score, int fps);

    MountController(int index, const std::string& mount_path);
    ~MountController();

    MountController(const MountController&) = delete;
    MountController& operator=(const MountController&) = delete;

    // Installs the media factory for this mount into |mounts| with the
    // given |launch| string and |transport| protocols. The factory is
    // shared (one pipeline per mount regardless of client count).
    void install(GstRTSPMountPoints* mounts, const std::string& launch,
                 GstRTSPLowerTrans transport);

    // Regenerates the launch string and re-arms the factory.
    void refresh_launch(const std::string& launch);

    // Sets a property on the live source element ("camsrc").
    // Returns false if no pipeline is live.
    bool set_source_property(const char* property, const char* value);

    // Returns the current stream status (frame count, fps, metadata).
    StreamStatus status() const;

    // Watchdog: checks if this mount's live pipeline is stalled.
    // Returns true if the stall threshold is exceeded (caller should
    // disable this mount).
    bool check_stall(double configured_fps);

    // Marks this mount dead after a stall: drops the live-pipeline weak
    // refs and stops reporting mounted(). Deliberately does NOT touch the
    // wedged pipeline -- tearing down a stalled pipeline SIGBUSed on
    // target. Recovery is a config reload or service restart.
    void disable();

    int index() const { return index_; }
    const std::string& mount_path() const { return mount_path_; }
    bool mounted() const { return factory_ != nullptr && !dead_; }

private:
    static GstPadProbeReturn on_payload_buffer(GstPad* pad,
                                               GstPadProbeInfo* info,
                                               gpointer user_data);
    static void on_media_configure(GstRTSPMediaFactory* factory,
                                   GstRTSPMedia* media, gpointer user_data);
    static void on_media_unprepared(GstRTSPMedia* media, gpointer user_data);

    int index_;
    std::string mount_path_;
    bool dead_ = false;  // stalled and disabled by the watchdog
    GstRTSPMediaFactory* factory_ = nullptr;  // ref held for refresh_launch
    GWeakRef media_;   // live GstRTSPMedia (shared factory: one at a time)
    GWeakRef source_;
    // Detection config, applied to each media as it is configured.
    detect::IMetaSink* meta_sink_ = nullptr;
    std::string detect_model_;
    int detect_width_ = 0, detect_height_ = 0, detect_fps_ = 10;
    double detect_score_ = 0.45;
    std::atomic<bool> detect_stop_{false};
    std::thread detect_thread_;
    void start_detection(GstElement* bin);
    void stop_detection();  // "camsrc" element inside the live pipeline
    std::atomic<guint64> frames_{0};
    // Last-frame metadata, written by the streaming thread's pad probe.
    std::atomic<guint64> last_sequence_{0};
    std::atomic<guint64> last_pts_{0};
    std::atomic<gint64> last_wallclock_{0};
    // Watchdog bookkeeping (main loop thread only).
    guint64 last_frames_ = 0;
    int stalled_checks_ = 0;
    std::atomic<double> fps_{0};
};

}  // namespace camera
