#include "camera/control/handlers/V4l2ControlHandler.h"

#include <string>

#include "camera/control/JsonUtil.h"

namespace camera {

HandlerResult ListControlsHandler::handle(JsonObject* params, ControlContext& ctx) {
    auto cam_idx = require_camera(params);
    if (!cam_idx) {
        return camera::base::makeUnexpected(cam_idx.error());
    }
    V4l2Device dev(ctx.config.cameras[*cam_idx].device);
    auto ctrls = dev.list_controls();
    if (!ctrls) {
        return camera::base::makeUnexpected(
            ControlError{kFailed, std::move(ctrls.error())});
    }
    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "controls");
    json_builder_begin_array(b);
    for (const V4l2Control& c : *ctrls)
        add_v4l2_control(b, c);
    json_builder_end_array(b);
    json_builder_end_object(b);
    return take_root(b);
}

HandlerResult GetControlHandler::handle(JsonObject* params, ControlContext& ctx) {
    auto cam_idx = require_camera(params);
    if (!cam_idx) {
        return camera::base::makeUnexpected(cam_idx.error());
    }
    std::string control;
    if (!param_control(params, &control)) {
        return camera::base::makeUnexpected(ControlError{kInvalidParams, "control must be a name or numeric id"});
    }
    V4l2Device dev(ctx.config.cameras[*cam_idx].device);
    auto c = dev.get_control(control);
    if (!c) {
        return camera::base::makeUnexpected(
            ControlError{kFailed, std::move(c.error())});
    }
    JsonBuilder* b = json_builder_new();
    add_v4l2_control(b, *c);
    return take_root(b);
}

HandlerResult SetControlHandler::handle(JsonObject* params, ControlContext& ctx) {
    auto cam_idx = require_camera(params);
    if (!cam_idx) {
        return camera::base::makeUnexpected(cam_idx.error());
    }
    std::string control;
    if (!param_control(params, &control)) {
        return camera::base::makeUnexpected(ControlError{kInvalidParams, "control must be a name or numeric id"});
    }
    int64_t value;
    if (!param_int(params, "value", &value)) {
        return camera::base::makeUnexpected(ControlError{kInvalidParams, "value must be an integer"});
    }
    V4l2Device dev(ctx.config.cameras[*cam_idx].device);
    auto r = dev.set_control(control, value);
    if (!r) {
        return camera::base::makeUnexpected(
            ControlError{kFailed, std::move(r.error())});
    }
    return empty_result();
}

}  // namespace camera
