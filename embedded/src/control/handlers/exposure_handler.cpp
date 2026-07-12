#include "control/handlers/exposure_handler.h"

#include <glib.h>

#include <string>

#include "control/json_util.h"

namespace {

// Shared logic for set-exposure and set-gain. |is_exposure| selects which
// parameter to read and which source method to call.
JsonNode* handle_exposure_or_gain(bool is_exposure, JsonObject* params,
                                  ControlContext& ctx, int* err_code,
                                  std::string* err_msg) {
    int cam_idx;
    if (!param_camera(params, &cam_idx)) {
        *err_code = kInvalidParams;
        *err_msg = "camera must be 0 or 1";
        return nullptr;
    }

    CameraConfig& cam = ctx.config.cameras[cam_idx];
    auto source = ctx.source_factory.create(cam.source);
    if (!source) {
        *err_code = kFailed;
        *err_msg = "unknown source '" + cam.source + "'";
        return nullptr;
    }

    if (is_exposure) {
        int64_t us;
        if (!param_int(params, "us", &us) || us < 0) {
            *err_code = kInvalidParams;
            *err_msg = "us must be an integer >= 0";
            return nullptr;
        }
        SourceResult r =
            source->set_exposure(cam_idx, cam, static_cast<int>(us), ctx.stream);
        if (!r.ok) {
            *err_code = kFailed;
            *err_msg = r.error;
            return nullptr;
        }
        g_message("control: cam%d exposure = %lld us", cam_idx,
                  static_cast<long long>(us));
    } else {
        double gain;
        if (!param_double(params, "gain", &gain) || gain < 0) {
            *err_code = kInvalidParams;
            *err_msg = "gain must be a number >= 0";
            return nullptr;
        }
        SourceResult r =
            source->set_gain(cam_idx, cam, gain, ctx.stream);
        if (!r.ok) {
            *err_code = kFailed;
            *err_msg = r.error;
            return nullptr;
        }
        g_message("control: cam%d gain = %g", cam_idx, gain);
    }

    return empty_result();
}

}  // namespace

JsonNode* SetExposureHandler::handle(JsonObject* params, ControlContext& ctx,
                                     int* err_code, std::string* err_msg) {
    return handle_exposure_or_gain(true, params, ctx, err_code, err_msg);
}

JsonNode* SetGainHandler::handle(JsonObject* params, ControlContext& ctx,
                                 int* err_code, std::string* err_msg) {
    return handle_exposure_or_gain(false, params, ctx, err_code, err_msg);
}
