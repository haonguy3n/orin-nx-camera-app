#include "camera/control/handlers/TriggerHandler.h"

#include <string>

#include "camera/control/JsonUtil.h"

#include "camera/base/logging/xlog.h"

namespace camera {

HandlerResult SetTriggerHandler::handle(JsonObject* params, ControlContext& ctx) {
    auto cam_idx = require_camera(params);
    if (!cam_idx) {
        return camera::base::makeUnexpected(cam_idx.error());
    }
    int64_t mode;
    if (!param_int(params, "mode", &mode) || mode < 0) {
        return camera::base::makeUnexpected(ControlError{kInvalidParams, "mode must be an integer >= 0"});
    }

    CameraConfig& cam = ctx.config.cameras[*cam_idx];
    auto source = create_source(cam.source);
    if (!source) {
        return camera::base::makeUnexpected(ControlError{kFailed, "unknown source '" + cam.source + "'"});
    }

    SourceResult r = source->set_trigger(cam, static_cast<int>(mode));
    if (!r) {
        return camera::base::makeUnexpected(ControlError{kFailed, std::move(r.error())});
    }
    XLOGF(INFO, "control: cam%d trigger mode = %d", *cam_idx, cam.trigger);
    return empty_result();
}

HandlerResult FireTriggerHandler::handle(JsonObject* params, ControlContext& ctx) {
    auto cam_idx = require_camera(params);
    if (!cam_idx) {
        return camera::base::makeUnexpected(cam_idx.error());
    }
    const CameraConfig& cam = ctx.config.cameras[*cam_idx];
    if (cam.source != "v4l2") {
        return camera::base::makeUnexpected(ControlError{kFailed, "software trigger requires the v4l2 source "
                   "(current source '" + cam.source + "')"});
    }

    auto r = V4l2Device(cam.device).fire_single_trigger();
    if (!r) {
        return camera::base::makeUnexpected(
            ControlError{kFailed, std::move(r.error())});
    }
    return empty_result();
}

HandlerResult SetSyncHandler::handle(JsonObject* params, ControlContext& ctx) {
    bool enabled;
    if (!param_bool(params, "enabled", &enabled)) {
        return camera::base::makeUnexpected(ControlError{kInvalidParams, "enabled must be a boolean"});
    }
    // All-or-nothing precheck: hardware sync only makes sense when every
    // enabled camera is on the v4l2 path.
    for (int i = 0; i < Config::kNumCameras; ++i) {
        const CameraConfig& cam = ctx.config.cameras[i];
        if (cam.enabled && cam.source != "v4l2") {
            return camera::base::makeUnexpected(ControlError{kFailed, "cam" + std::to_string(i) + " is source '" +
                       cam.source + "'; sync requires v4l2"});
        }
    }
    const int mode = enabled ? 1 : 0;  // external / free running
    for (int i = 0; i < Config::kNumCameras; ++i) {
        CameraConfig& cam = ctx.config.cameras[i];
        if (!cam.enabled)
            continue;
        auto r = V4l2Device(cam.device).set_trigger_mode(mode);
        if (!r) {
            return camera::base::makeUnexpected(
                ControlError{kFailed, std::move(r.error())});
        }
        cam.trigger = mode;
    }
    XLOGF(INFO, "control: sync trigger %s", enabled ? "on" : "off");
    return empty_result();
}

}  // namespace camera
