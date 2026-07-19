#include "camera/control/handlers/ExposureHandler.h"

#include <glib.h>

#include <string>

#include "camera/control/JsonUtil.h"

#include "camera/base/logging/xlog.h"

namespace camera {

namespace {

// Shared logic for set-exposure and set-gain. |is_exposure| selects which
// parameter to read and which source method to call.
HandlerResult handle_exposure_or_gain(bool is_exposure, JsonObject* params,
                                      ControlContext& ctx) {
    auto cam_idx = require_camera(params);
    if (!cam_idx) {
        return camera::base::makeUnexpected(cam_idx.error());
    }

    CameraConfig& cam = ctx.config.cameras[*cam_idx];
    auto source = create_source(cam.source);
    if (!source) {
        return camera::base::makeUnexpected(ControlError{kFailed, "unknown source '" + cam.source + "'"});
    }

    if (is_exposure) {
        int64_t us;
        if (!param_int(params, "us", &us) || us < 0) {
            return camera::base::makeUnexpected(ControlError{kInvalidParams, "us must be an integer >= 0"});
        }
        SourceResult r = source->set_exposure(*cam_idx, cam,
                                              static_cast<int>(us), ctx.stream);
        if (!r) {
            return camera::base::makeUnexpected(ControlError{kFailed, std::move(r.error())});
        }
        XLOGF(INFO, "control: cam%d exposure = %lld us", *cam_idx,
                  static_cast<long long>(us));
    } else {
        double gain;
        if (!param_double(params, "gain", &gain) || gain < 0) {
            return camera::base::makeUnexpected(ControlError{kInvalidParams, "gain must be a number >= 0"});
        }
        SourceResult r =
            source->set_gain(*cam_idx, cam, gain, ctx.stream);
        if (!r) {
            return camera::base::makeUnexpected(ControlError{kFailed, std::move(r.error())});
        }
        XLOGF(INFO, "control: cam%d gain = %g", *cam_idx, gain);
    }

    return empty_result();
}

}  // namespace

HandlerResult SetExposureHandler::handle(JsonObject* params, ControlContext& ctx) {
    return handle_exposure_or_gain(true, params, ctx);
}

HandlerResult SetGainHandler::handle(JsonObject* params, ControlContext& ctx) {
    return handle_exposure_or_gain(false, params, ctx);
}

}  // namespace camera
