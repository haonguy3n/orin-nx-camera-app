// Argus (ISP) camera source strategy.
//
// Uses nvarguscamerasrc for RAW-through-Tegra-ISP capture: debayer, 3A,
// NV12 in NVMM (zero-copy into NVENC). Default for color IMX296C modules.
// Runtime exposure/gain are applied as nvarguscamerasrc range properties
// (min==max pins the value; wide range = auto).
#pragma once

#include "pipeline/camera_source.h"

class ArgusSource : public ICameraSource {
public:
    std::string source_type() const override { return "argus"; }
    std::string build_launch(const CameraConfig& cam) const override;
    void apply_initial_settings(const CameraConfig& cam) const override;

    SourceResult set_exposure(int cam_index, CameraConfig& cam, int us,
                              IStreamController& stream) const override;
    SourceResult set_gain(int cam_index, CameraConfig& cam, double gain,
                          IStreamController& stream) const override;
    SourceResult set_trigger(CameraConfig& cam, int mode,
                             IV4l2DeviceFactory& v4l2) const override;
    SourceResult set_isp(int cam_index, CameraConfig& cam,
                         const std::string& param,
                         const std::string& value,
                         IStreamController& stream) const override;

    bool supports_trigger() const override { return false; }
    bool supports_isp() const override { return true; }
};
