// Test pattern camera source strategy.
//
// Uses videotestsrc with a software encoder (x264enc/x265enc). Runs on
// any host with standard GStreamer plugins -- no hardware needed. Used
// for CI, development, and the config/test-videotestsrc.conf smoke test.
// No runtime sensor controls are supported.
#pragma once

#include "camera/pipeline/CameraSource.h"

namespace camera {

class TestSource : public ICameraSource {
public:
    std::string source_type() const override { return "test"; }
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

    bool supports_trigger() const override { return false; }
    bool supports_isp() const override { return false; }
};

}  // namespace camera
