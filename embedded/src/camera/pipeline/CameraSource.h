// Camera source strategy interface (Strategy pattern).
//
// Each concrete strategy encapsulates everything source-specific:
//  - how to build the capture fragment for that source type
//  - how to apply initial sensor settings at startup
//  - how to apply runtime exposure/gain/trigger/ISP changes
//  - what capabilities the source supports
//
// This replaces the source-specific if-else chains in pipeline construction
// and control dispatch. PipelineBuilder composes these fragments for RTSP;
// media::CameraPipeline composes them for direct encoded-frame transports.
#pragma once

#include <json-glib/json-glib.h>

#include <string>

#include "camera/config/Config.h"
#include "camera/base/Expected.h"
#include "camera/base/Unit.h"

namespace camera {

class IStreamController;  // from core/StreamController.h

// Result of a runtime setting application: camera::base::unit on success,
// error message on failure.
using SourceResult = camera::base::Expected<camera::base::Unit, std::string>;

class ICameraSource {
public:
    virtual ~ICameraSource() = default;

    // Source type identifier: "argus", "v4l2", "test".
    virtual std::string source_type() const = 0;

    // Whether the encoded graph should use the platform encoder. Test sources
    // override this for portable development builds; new source strategies do
    // not require transport-specific type checks.
    virtual bool uses_hardware_encoder() const { return true; }

    // The capture part -- the bare fragment ending right before the
    // encoder, "camsrc" inside, no "( )". This is what the usb transport's
    // media::PipelineSpec carries; the typed encode/detect chain is built
    // onto it programmatically (media::CameraPipeline::build).
    virtual std::string build_source_fragment(const CameraConfig& cam)
        const = 0;

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
    virtual SourceResult set_trigger(CameraConfig& cam, int mode) const = 0;

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

}  // namespace camera
