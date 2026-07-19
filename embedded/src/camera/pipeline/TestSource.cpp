#include "camera/pipeline/TestSource.h"

#include <string>

#include "camera/core/StreamController.h"
#include "camera/pipeline/PipelineBuilder.h"

namespace camera {

std::string TestSource::build_source_fragment(const CameraConfig& cam) const {
    return "videotestsrc name=camsrc is-live=true ! video/x-raw," +
           PipelineBuilder::caps_tail(cam) + " ! videoconvert";
}

void TestSource::apply_initial_settings(const CameraConfig& /*cam*/) const {
    // No sensor to configure.
}

SourceResult TestSource::set_exposure(int /*cam_index*/, CameraConfig& /*cam*/,
                                      int /*us*/,
                                      IStreamController& /*stream*/) const {
    return camera::base::makeUnexpected(std::string("not supported for source 'test'"));
}

SourceResult TestSource::set_gain(int /*cam_index*/, CameraConfig& /*cam*/,
                                  double /*gain*/,
                                  IStreamController& /*stream*/) const {
    return camera::base::makeUnexpected(std::string("not supported for source 'test'"));
}

SourceResult TestSource::set_trigger(CameraConfig& /*cam*/, int /*mode*/) const {
    return camera::base::makeUnexpected(std::string("not supported for source 'test'"));
}

SourceResult TestSource::set_isp(int /*cam_index*/, CameraConfig& /*cam*/,
                                 const std::string& /*param*/,
                                 const std::string& /*value*/,
                                 IStreamController& /*stream*/) const {
    return camera::base::makeUnexpected(std::string("not supported for source 'test'"));
}

}  // namespace camera
