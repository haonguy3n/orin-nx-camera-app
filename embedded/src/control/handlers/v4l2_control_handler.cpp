#include "control/handlers/v4l2_control_handler.h"

#include <string>

#include "control/json_util.h"

JsonNode* ListControlsHandler::handle(JsonObject* params, ControlContext& ctx,
                                      int* err_code, std::string* err_msg) {
    int cam_idx;
    if (!param_camera(params, &cam_idx)) {
        *err_code = kInvalidParams;
        *err_msg = "camera must be 0 or 1";
        return nullptr;
    }
    auto dev = ctx.v4l2_factory.open(ctx.config.cameras[cam_idx].device);
    if (!dev) {
        *err_code = kFailed;
        *err_msg = ctx.config.cameras[cam_idx].device + ": cannot open device";
        return nullptr;
    }
    std::string err;
    std::vector<V4l2Control> ctrls = dev->list_controls(&err);
    if (ctrls.empty()) {
        *err_code = kFailed;
        *err_msg = err;
        return nullptr;
    }
    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "controls");
    json_builder_begin_array(b);
    for (const V4l2Control& c : ctrls)
        add_v4l2_control(b, c);
    json_builder_end_array(b);
    json_builder_end_object(b);
    return take_root(b);
}

JsonNode* GetControlHandler::handle(JsonObject* params, ControlContext& ctx,
                                    int* err_code, std::string* err_msg) {
    int cam_idx;
    std::string control;
    if (!param_camera(params, &cam_idx)) {
        *err_code = kInvalidParams;
        *err_msg = "camera must be 0 or 1";
        return nullptr;
    }
    if (!param_control(params, &control)) {
        *err_code = kInvalidParams;
        *err_msg = "control must be a name or numeric id";
        return nullptr;
    }
    auto dev = ctx.v4l2_factory.open(ctx.config.cameras[cam_idx].device);
    if (!dev) {
        *err_code = kFailed;
        *err_msg = ctx.config.cameras[cam_idx].device + ": cannot open device";
        return nullptr;
    }
    V4l2Control c;
    std::string err;
    if (!dev->get_control(control, &c, &err)) {
        *err_code = kFailed;
        *err_msg = err;
        return nullptr;
    }
    JsonBuilder* b = json_builder_new();
    add_v4l2_control(b, c);
    return take_root(b);
}

JsonNode* SetControlHandler::handle(JsonObject* params, ControlContext& ctx,
                                    int* err_code, std::string* err_msg) {
    int cam_idx;
    std::string control;
    if (!param_camera(params, &cam_idx)) {
        *err_code = kInvalidParams;
        *err_msg = "camera must be 0 or 1";
        return nullptr;
    }
    if (!param_control(params, &control)) {
        *err_code = kInvalidParams;
        *err_msg = "control must be a name or numeric id";
        return nullptr;
    }
    int64_t value;
    if (!param_int(params, "value", &value)) {
        *err_code = kInvalidParams;
        *err_msg = "value must be an integer";
        return nullptr;
    }
    auto dev = ctx.v4l2_factory.open(ctx.config.cameras[cam_idx].device);
    if (!dev) {
        *err_code = kFailed;
        *err_msg = ctx.config.cameras[cam_idx].device + ": cannot open device";
        return nullptr;
    }
    std::string err;
    if (!dev->set_control(control, value, &err)) {
        *err_code = kFailed;
        *err_msg = err;
        return nullptr;
    }
    return empty_result();
}
