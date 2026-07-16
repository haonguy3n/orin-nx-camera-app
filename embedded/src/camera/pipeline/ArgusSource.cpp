#include "camera/pipeline/ArgusSource.h"

#include <glib.h>

#include <string>

#include "camera/core/StreamController.h"
#include "camera/pipeline/PipelineBuilder.h"

namespace camera {

namespace {

// "Auto" ranges handed to nvarguscamerasrc when exposure/gain is set back
// to 0: wide enough to give the 3A loop its freedom back, clamped by Argus
// to the sensor's real limits (there is no property to truly unset a range).
constexpr const char* kAutoExposure = "13000 100000000";  // 13 us..100 ms
constexpr const char* kAutoGain = "1 16";

}  // namespace

std::string ArgusSource::build_launch(const CameraConfig& cam) const {
    std::string p = "( ";
    p += "nvarguscamerasrc name=camsrc sensor-id=" +
         std::to_string(cam.sensor_id) +
         PipelineBuilder::argus_ranges(cam) +
         " ! video/x-raw(memory:NVMM)," + PipelineBuilder::caps_tail(cam);
    if (cam.zoom > 1.0)
        p += PipelineBuilder::zoom_tail(cam);
    p += " ! " + PipelineBuilder::nvenc_tail(cam);
    p += " )";
    return p;
}

void ArgusSource::apply_initial_settings(const CameraConfig& /*cam*/) const {
    // Argus settings go into the launch string; nothing to do at startup.
}

SourceResult ArgusSource::set_exposure(int cam_index, CameraConfig& cam, int us,
                                       IStreamController& stream) const {
    std::string range;
    if (us > 0) {
        const std::string ns = std::to_string(static_cast<long long>(us) * 1000);
        range = ns + " " + ns;
    } else {
        range = kAutoExposure;
    }
    // No live pipeline is fine: the config below seeds the launch string
    // of the next pipeline.
    stream.set_source_property(cam_index, "exposuretimerange", range.c_str());
    cam.exposure = us;
    stream.refresh_launch(cam_index);
    return folly::unit;
}

SourceResult ArgusSource::set_gain(int cam_index, CameraConfig& cam, double gain,
                                   IStreamController& stream) const {
    std::string range;
    if (gain > 0) {
        char buf[32];
        g_snprintf(buf, sizeof(buf), "%g %g", gain, gain);
        range = buf;
    } else {
        range = kAutoGain;
    }
    stream.set_source_property(cam_index, "gainrange", range.c_str());
    cam.gain = gain;
    stream.refresh_launch(cam_index);
    return folly::unit;
}

SourceResult ArgusSource::set_trigger(CameraConfig& /*cam*/, int /*mode*/,
                                      IV4l2DeviceFactory& /*v4l2*/) const {
    return folly::makeUnexpected(std::string("hardware trigger requires the v4l2 source "
                   "(current source 'argus')"));
}

SourceResult ArgusSource::set_isp(int cam_index, CameraConfig& cam,
                                  const std::string& param,
                                  const std::string& value,
                                  IStreamController& stream) const {
    if (value.empty()) {
        cam.isp.erase(param);
        stream.refresh_launch(cam_index);
        return folly::unit;
    }
    cam.isp[param] = value;
    // Live pipeline picks it up now; the refreshed factory launch string
    // covers every session created afterwards.
    stream.set_source_property(cam_index, param.c_str(), value.c_str());
    stream.refresh_launch(cam_index);
    return folly::unit;
}

}  // namespace camera
