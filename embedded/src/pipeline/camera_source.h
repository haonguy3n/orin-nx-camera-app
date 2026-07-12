// Camera source strategy interface (Strategy pattern).
//
// Each concrete strategy encapsulates everything source-specific:
//  - how to build the GStreamer launch string for that source type
//  - how to apply initial sensor settings at startup
//  - how to apply runtime exposure/gain/trigger/ISP changes
//  - what capabilities the source supports
//
// This replaces the if-else chains in the original build_launch() and
// control dispatch, satisfying OCP (new source type = new class, no
// existing code modified) and SRP (each source class has one reason to
// change).
#pragma once

#include <json-glib/json-glib.h>

#include <string>

#include "config/config.h"

class IStreamController;  // from core/stream_controller.h
class IV4l2DeviceFactory; // from v4l2/v4l2_device.h

// Result of a runtime setting application: empty string on success,
// error message on failure.
struct SourceResult {
    bool ok = true;
    std::string error;
};

class ICameraSource {
public:
    virtual ~ICameraSource() = default;

    // Source type identifier: "argus", "v4l2", "test".
    virtual std::string source_type() const = 0;

    // Full gst_parse_launch pipeline string for this source, wrapped in ( )
    // as required by gst_rtsp_media_factory_set_launch. The source element
    // is always named "camsrc" so the stream controller can reach it live.
    virtual std::string build_launch(const CameraConfig& cam) const = 0;

    // Applies initial sensor settings (exposure, gain, trigger) at startup.
    // For argus/test this is a no-op (settings go into the launch string);
    // for v4l2 it writes V4L2 controls directly to the device.
    virtual void apply_initial_settings(const CameraConfig& cam) const = 0;

    // Runtime setting: exposure in microseconds (0 = auto/default).
    // |cam_index| identifies which mount to operate on. Updates |cam| and
    // applies to the live pipeline via |stream| if live.
    virtual SourceResult set_exposure(int cam_index, CameraConfig& cam, int us,
                                      IStreamController& stream) const = 0;

    // Runtime setting: analog gain (0 = auto/default).
    virtual SourceResult set_gain(int cam_index, CameraConfig& cam, double gain,
                                  IStreamController& stream) const = 0;

    // Runtime setting: hardware trigger mode (0..7).
    // Only meaningful for v4l2 sources; returns error for others.
    virtual SourceResult set_trigger(CameraConfig& cam, int mode,
                                     IV4l2DeviceFactory& v4l2) const = 0;

    // Runtime setting: nvarguscamerasrc ISP property.
    // Only meaningful for argus sources; returns error for others.
    // Empty |value| means "forget the override" (erase from config).
    virtual SourceResult set_isp(int cam_index, CameraConfig& cam,
                                 const std::string& param,
                                 const std::string& value,
                                 IStreamController& stream) const = 0;

    // Capability queries (used by control handlers for validation).
    virtual bool supports_trigger() const = 0;
    virtual bool supports_isp() const = 0;
};
