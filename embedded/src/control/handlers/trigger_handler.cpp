#include "control/handlers/trigger_handler.h"

#include <string>

#include "control/json_util.h"

JsonNode* SetTriggerHandler::handle(JsonObject* params, ControlContext& ctx,
                                    int* err_code, std::string* err_msg) {
    int cam_idx;
    if (!param_camera(params, &cam_idx)) {
        *err_code = kInvalidParams;
        *err_msg = "camera must be 0 or 1";
        return nullptr;
    }
    int64_t mode;
    if (!param_int(params, "mode", &mode) || mode < 0) {
        *err_code = kInvalidParams;
        *err_msg = "mode must be an integer >= 0";
        return nullptr;
    }

    CameraConfig& cam = ctx.config.cameras[cam_idx];
    auto source = ctx.source_factory.create(cam.source);
    if (!source) {
        *err_code = kFailed;
        *err_msg = "unknown source '" + cam.source + "'";
        return nullptr;
    }

    SourceResult r = source->set_trigger(cam, static_cast<int>(mode),
                                         ctx.v4l2_factory);
    if (!r.ok) {
        *err_code = kFailed;
        *err_msg = r.error;
        return nullptr;
    }
    g_message("control: cam%d trigger mode = %d", cam_idx, cam.trigger);
    return empty_result();
}

JsonNode* FireTriggerHandler::handle(JsonObject* params, ControlContext& ctx,
                                     int* err_code, std::string* err_msg) {
    int cam_idx;
    if (!param_camera(params, &cam_idx)) {
        *err_code = kInvalidParams;
        *err_msg = "camera must be 0 or 1";
        return nullptr;
    }
    const CameraConfig& cam = ctx.config.cameras[cam_idx];
    if (cam.source != "v4l2") {
        *err_code = kFailed;
        *err_msg = "software trigger requires the v4l2 source "
                   "(current source '" + cam.source + "')";
        return nullptr;
    }

    auto dev = ctx.v4l2_factory.open(cam.device);
    if (!dev) {
        *err_code = kFailed;
        *err_msg = cam.device + ": cannot open device";
        return nullptr;
    }
    std::string err;
    if (!dev->fire_single_trigger(&err)) {
        *err_code = kFailed;
        *err_msg = err;
        return nullptr;
    }
    return empty_result();
}

JsonNode* SetSyncHandler::handle(JsonObject* params, ControlContext& ctx,
                                 int* err_code, std::string* err_msg) {
    bool enabled;
    if (!param_bool(params, "enabled", &enabled)) {
        *err_code = kInvalidParams;
        *err_msg = "enabled must be a boolean";
        return nullptr;
    }
    // All-or-nothing precheck: hardware sync only makes sense when every
    // enabled camera is on the v4l2 path.
    for (int i = 0; i < Config::kNumCameras; ++i) {
        const CameraConfig& cam = ctx.config.cameras[i];
        if (cam.enabled && cam.source != "v4l2") {
            *err_code = kFailed;
            *err_msg = "cam" + std::to_string(i) + " is source '" +
                       cam.source + "'; sync requires v4l2";
            return nullptr;
        }
    }
    const int mode = enabled ? 1 : 0;  // external / free running
    for (int i = 0; i < Config::kNumCameras; ++i) {
        CameraConfig& cam = ctx.config.cameras[i];
        if (!cam.enabled)
            continue;
        auto dev = ctx.v4l2_factory.open(cam.device);
        if (!dev) {
            *err_code = kFailed;
            *err_msg = cam.device + ": cannot open device";
            return nullptr;
        }
        std::string err;
        if (!dev->set_trigger_mode(mode, &err)) {
            *err_code = kFailed;
            *err_msg = err;
            return nullptr;
        }
        cam.trigger = mode;
    }
    g_message("control: sync trigger %s", enabled ? "on" : "off");
    return empty_result();
}
