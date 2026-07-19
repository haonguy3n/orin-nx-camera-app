#include "camera/pipeline/V4l2Source.h"

#include "camera/lib/v4l2/V4l2Device.h"

#include <glib.h>

#include <string>

#include "camera/core/StreamController.h"
#include "camera/pipeline/PipelineBuilder.h"

#include "camera/base/logging/xlog.h"

namespace camera {

std::string V4l2Source::build_source_fragment(const CameraConfig& cam) const {
    // GRAY8 explicitly: the mono IMX296's native 10-bit grey (Y10) has
    // no v4l2src mapping, so an unconstrained video/x-raw fails caps
    // negotiation (mount answers 503). The VC driver also serves 8-bit
    // grey, which v4l2src and nvvidconv both handle; nvvidconv converts
    // into NVMM NV12 for the encoder (and crops when zoomed).
    return "v4l2src name=camsrc device=" + cam.device +
           " ! video/x-raw,format=GRAY8," + PipelineBuilder::caps_tail(cam) +
           PipelineBuilder::zoom_tail(cam);
}

std::string V4l2Source::build_launch(const CameraConfig& cam) const {
    return "( " + build_source_fragment(cam) + " ! " +
           PipelineBuilder::nvenc_tail(cam) + " )";
}

void V4l2Source::apply_initial_settings(const CameraConfig& cam) const {
    // Config sensor settings for the v4l2 path go straight to the VC
    // driver's V4L2 controls (no pipeline needed). Failures are warnings:
    // the device may legitimately lack a control, or not exist on a
    // development host.
    V4l2Device dev(cam.device);
    if (cam.exposure > 0) {
        if (auto r = dev.set_control("exposure", cam.exposure); !r)
            XLOGF(WARN, "v4l2: %s", r.error().c_str());
    }
    if (cam.gain > 0) {
        if (auto r = dev.set_control("gain",
                                      static_cast<int64_t>(cam.gain));
            !r)
            XLOGF(WARN, "v4l2: %s", r.error().c_str());
    }
    if (cam.trigger >= 0) {
        if (auto r = dev.set_trigger_mode(cam.trigger); !r)
            XLOGF(WARN, "v4l2: %s", r.error().c_str());
    }
}

SourceResult V4l2Source::set_exposure(int /*cam_index*/, CameraConfig& cam,
                                      int us,
                                      IStreamController& /*stream*/) const {
    V4l2Device dev(cam.device);
    if (us == 0) {  // 0 = back to the driver default
        auto c = dev.get_control("exposure");
        if (!c)
            return camera::base::makeUnexpected(std::move(c.error()));
        if (auto r = dev.set_control("exposure", c->default_value); !r)
            return r;
    } else {
        if (auto r = dev.set_control("exposure", us); !r)
            return r;
    }
    cam.exposure = us;
    return camera::base::unit;
}

SourceResult V4l2Source::set_gain(int /*cam_index*/, CameraConfig& cam,
                                  double gain,
                                  IStreamController& /*stream*/) const {
    V4l2Device dev(cam.device);
    if (gain == 0) {  // 0 = back to the driver default
        auto c = dev.get_control("gain");
        if (!c)
            return camera::base::makeUnexpected(std::move(c.error()));
        if (auto r = dev.set_control("gain", c->default_value); !r)
            return r;
    } else {
        if (auto r = dev.set_control("gain", static_cast<int64_t>(gain));
            !r)
            return r;
    }
    cam.gain = gain;
    return camera::base::unit;
}

SourceResult V4l2Source::set_trigger(CameraConfig& cam, int mode) const {
    V4l2Device dev(cam.device);
    if (auto r = dev.set_trigger_mode(mode); !r)
        return r;
    cam.trigger = mode;
    return camera::base::unit;
}

SourceResult V4l2Source::set_isp(int /*cam_index*/, CameraConfig& /*cam*/,
                                 const std::string& /*param*/,
                                 const std::string& /*value*/,
                                 IStreamController& /*stream*/) const {
    return camera::base::makeUnexpected(std::string(
        "ISP controls require the argus source (current source 'v4l2')"));
}

}  // namespace camera
