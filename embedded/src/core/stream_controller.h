// Stream controller interface (abstraction over the RTSP server).
//
// This interface decouples the control handlers and camera source
// strategies from the concrete RtspServer class. It exposes only the
// operations they need: live pipeline manipulation (set source property,
// refresh launch string) and status queries (bound address, client count,
// per-camera stream status). This enables unit-testing handlers with a
// mock stream controller.
#pragma once

#include <cstdint>
#include <string>

// Runtime state of one mount, for get-status and the watchdog.
struct StreamStatus {
    bool mounted = false;    // camera enabled, factory installed
    bool streaming = false;  // a (shared) media pipeline is currently live
    guint64 frames = 0;      // buffers through the payloader since start()
    double fps = 0;          // measured over the last watchdog period --
                             // the rate actually delivered, which is not the
                             // configured one when Argus AE trades frame rate
                             // for exposure in dim light
    // Metadata of the newest frame (valid once frames > 0): capture
    // sequence (v4l2 frame sequence when the source provides it), buffer
    // PTS in ns, and the wallclock us when it passed the payloader.
    guint64 sequence = 0;
    guint64 pts = 0;
    gint64 wallclock = 0;
};

class IStreamController {
public:
    virtual ~IStreamController() = default;

    virtual const std::string& bound_address() const = 0;
    virtual int client_count() const = 0;
    virtual StreamStatus stream_status(int cam) = 0;

    // Sets a property on camera |cam|'s live source element (argus runtime
    // exposure/gain -- nvarguscamerasrc forwards range properties while
    // streaming). |value| is converted per gst_util_set_object_arg.
    // Returns false if no pipeline is live; the caller keeps the config
    // updated so later pipelines pick the value up from the launch string.
    virtual bool set_source_property(int cam, const char* property,
                                     const char* value) = 0;

    // Regenerates camera |cam|'s launch string from the (mutated) config
    // and re-arms its media factory with it. Without this, runtime settings
    // (set-isp / argus exposure/gain) would only reach the currently-live
    // pipeline: the factory keeps the launch string from start(), so later
    // sessions would revert. Takes effect for the next created media.
    virtual void refresh_launch(int cam) = 0;
};
