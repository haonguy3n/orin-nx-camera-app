// V4L2 camera source strategy.
//
// Uses v4l2src for RAW capture straight from VI (no ISP). Right choice for
// mono IMX296 (no debayer needed) and for trigger-synchronized machine-
// vision capture. Sensor settings (exposure, gain, trigger) go directly to
// the VC driver's V4L2 controls via V4l2Device.
#pragma once

#include "camera/pipeline/CameraSource.h"

namespace camera {

class V4l2Source : public ICameraSource {
public:
    std::string source_type() const override { return "v4l2"; }
    std::string build_launch(const CameraConfig& cam) const override;
    std::string build_source_fragment(const CameraConfig& cam) const override;
    void apply_initial_settings(const CameraConfig& cam) const override;

    SourceResult set_exposure(int cam_index, CameraConfig& cam, int us,
                              IStreamController& stream) const override;
    SourceResult set_gain(int cam_index, CameraConfig& cam, double gain,
                          IStreamController& stream) const override;
    SourceResult set_trigger(CameraConfig& cam, int mode) const override;
    SourceResult set_isp(int cam_index, CameraConfig& cam,
                         const std::string& param,
                         const std::string& value,
                         IStreamController& stream) const override;

    bool supports_trigger() const override { return true; }
    bool supports_isp() const override { return false; }
};

}  // namespace camera
