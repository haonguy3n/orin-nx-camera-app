#include "pipeline/test_source.h"

#include <string>

#include "core/stream_controller.h"
#include "pipeline/pipeline_builder.h"

std::string TestSource::build_launch(const CameraConfig& cam) const {
    const bool h265 = cam.codec == "h265";
    std::string p = "( ";
    p += "videotestsrc name=camsrc is-live=true ! video/x-raw," +
         PipelineBuilder::caps_tail(cam) + " ! videoconvert ! queue ! ";
    // x26xenc bitrate is in kbit/s.
    p += std::string(h265 ? "x265enc" : "x264enc") +
         " tune=zerolatency key-int-max=30 bitrate=" +
         std::to_string(cam.bitrate / 1000) + " ! ";
    p += h265 ? "h265parse" : "h264parse";
    p += " ! queue ! ";
    p += h265 ? "rtph265pay" : "rtph264pay";
    p += " name=pay0 pt=96";
    p += " )";
    return p;
}

void TestSource::apply_initial_settings(const CameraConfig& /*cam*/) const {
    // No sensor to configure.
}

SourceResult TestSource::set_exposure(int /*cam_index*/, CameraConfig& /*cam*/,
                                      int /*us*/,
                                      IStreamController& /*stream*/) const {
    return {false, "not supported for source 'test'"};
}

SourceResult TestSource::set_gain(int /*cam_index*/, CameraConfig& /*cam*/,
                                  double /*gain*/,
                                  IStreamController& /*stream*/) const {
    return {false, "not supported for source 'test'"};
}

SourceResult TestSource::set_trigger(CameraConfig& /*cam*/, int /*mode*/,
                                     IV4l2DeviceFactory& /*v4l2*/) const {
    return {false, "not supported for source 'test'"};
}

SourceResult TestSource::set_isp(int /*cam_index*/, CameraConfig& /*cam*/,
                                 const std::string& /*param*/,
                                 const std::string& /*value*/,
                                 IStreamController& /*stream*/) const {
    return {false, "not supported for source 'test'"};
}
