// One camera's capture+encode pipeline, shared by every transport.
//
// Capture is identical for all of them -- source, encoder, appsink -- and only
// the delivery differs (encrypt to a USB endpoint, write to a socket, ...).
// That shared half lives here; transports implement IFrameTransport and are
// fanned out to.
//
// This is not only deduplication. Argus permits ONE consumer per camera, so
// two transports cannot each open the sensor: today the secure USB path falls
// back to pulling the camera's own RTSP mount back through loopback when RTSP
// got there first (see SecureUsbServer::video_description). One pipeline per
// camera with a fanout removes that fallback -- and with it the RTP
// payload/depayload round trip and the silent loss of the detect branch that
// the fallback caused.
#pragma once

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "camera/base/Expected.h"
#include "camera/base/Unit.h"

namespace camera::media {

// One encoded access unit as it leaves the encoder.
struct Frame {
    const uint8_t* data = nullptr;
    size_t size = 0;
    uint64_t pts = 0;      // GST_BUFFER_PTS, ns (GST_CLOCK_TIME_NONE if unset)
    bool keyframe = false;  // not a DELTA_UNIT
};

// A delivery strategy: secure USB, TCP, file, ... Called on the pipeline's pump
// thread, once per frame, with `frame` valid only for the duration of the call
// (the buffer is unmapped on return) -- copy anything you keep.
class IFrameTransport {
public:
    virtual ~IFrameTransport() = default;
    virtual void on_frame(uint8_t camera, const Frame& frame) = 0;
    // Optional: raw BGRx from the detection branch, when the description has
    // one. Default ignores it, so transports that do not care need no code.
    virtual void on_raw(uint8_t /*camera*/, const uint8_t* /*bgrx*/,
                        int /*width*/, int /*height*/, int /*stride*/) {}
};

using PipelineResult = base::Expected<base::Unit, std::string>;

class CameraPipeline {
public:
    CameraPipeline(uint8_t camera, std::string description);
    ~CameraPipeline();

    CameraPipeline(const CameraPipeline&) = delete;
    CameraPipeline& operator=(const CameraPipeline&) = delete;

    // Transports must be registered before start() and outlive the pipeline.
    void add_transport(IFrameTransport* transport);

    // Builds and plays. Fails with the state actually reached rather than
    // stalling silently: set_state returns ASYNC for a live source, so "no
    // error" never meant "running", and a branch that fails to preroll leaves
    // the whole pipeline in PAUSED looking exactly like a dead camera.
    PipelineResult start(guint64 timeout_ns = 10ULL * GST_SECOND);
    void stop();

    // Pulls at most one ENCODED frame and fans it out. Returns false on
    // timeout, so the caller's loop stays responsive to shutdown.
    bool pump(int timeout_ms);

    // Pulls at most one RAW frame from the detection branch and fans it out via
    // on_raw. Call from a SEPARATE thread to pump(): inference is ~25 ms while
    // video arrives every ~16 ms, so running both on one thread would stall the
    // video the host is watching behind the detector. Keeping them apart is why
    // the existing transport runs a dedicated detection thread, and that
    // property has to survive the migration.
    //
    // Returns false when there is no detect branch or nothing arrived in time.
    bool pump_raw(int timeout_ms);

    [[nodiscard]] bool has_detect_branch() const { return detect_ != nullptr; }

    // True once the encoded branch has signalled end-of-stream. Distinguishes
    // "camera went away" from "nothing arrived in this timeout", which decides
    // whether a caller should rebuild or keep waiting.
    [[nodiscard]] bool is_eos() const;

    // Runtime property on the live source element (argus forwards its range
    // properties while streaming). False when nothing is up.
    bool set_source_property(const char* property, const char* value);

    // Replaces the description and asks the owner to rebuild: anything the
    // source cannot change live (zoom is a crop) needs a new pipeline, and
    // without this a setting would not reach the next session either.
    void set_description(std::string description);
    bool relaunch_wanted() const { return relaunch_.load(); }
    std::string description() const;

    uint64_t frames() const { return frames_.load(); }

    // Drains ERROR/EOS, reporting once. A mount that never negotiates should
    // say so instead of looking like a camera producing nothing.
    void drain_bus();

private:
    void deliver(GstSample* sample);
    void deliver_raw(GstSample* sample);

    uint8_t camera_;
    mutable std::mutex mutex_;
    std::string description_;
    std::atomic<bool> relaunch_{false};
    std::atomic<uint64_t> frames_{0};

    GstElement* pipeline_ = nullptr;
    GstAppSink* sink_ = nullptr;
    GstAppSink* detect_ = nullptr;
    GstElement* source_ = nullptr;  // live element for set_source_property
    std::vector<IFrameTransport*> transports_;
};

}  // namespace camera::media
