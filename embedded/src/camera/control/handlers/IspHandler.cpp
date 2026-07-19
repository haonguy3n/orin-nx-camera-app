#include "camera/control/handlers/IspHandler.h"

#include <glib.h>

#include <string>

#include "camera/control/JsonUtil.h"

#include "camera/base/logging/xlog.h"

namespace camera {

namespace {

// nvarguscamerasrc properties settable via set-isp (docs/PROTOCOL.md).
// Everything else on the element (sensor-id, ranges, ...) is owned by the
// launch string / dedicated methods and must not be poked freely.
const char* const kIspParams[] = {
    "wbmode",         "saturation",  "tnr-mode",
    "tnr-strength",   "ee-mode",     "ee-strength",
    "aeantibanding",  "exposurecompensation",
    "aelock",         "awblock",     "ispdigitalgainrange",
};

bool is_isp_param(const std::string& name) {
    for (const char* p : kIspParams)
        if (name == p)
            return true;
    return false;
}

}  // namespace

HandlerResult SetIspHandler::handle(JsonObject* params, ControlContext& ctx) {
    auto cam_idx = require_camera(params);
    if (!cam_idx) {
        return camera::base::makeUnexpected(cam_idx.error());
    }
    CameraConfig& cam = ctx.config.cameras[*cam_idx];
    if (cam.source != "argus") {
        return camera::base::makeUnexpected(ControlError{kFailed, "ISP controls require the argus source "
                   "(current source '" + cam.source + "')"});
    }

    std::string name;
    if (params != nullptr && json_object_has_member(params, "param")) {
        JsonNode* n = json_object_get_member(params, "param");
        if (JSON_NODE_HOLDS_VALUE(n) &&
            json_node_get_value_type(n) == G_TYPE_STRING)
            name = json_node_get_string(n);
    }
    if (!is_isp_param(name)) {
        return camera::base::makeUnexpected(ControlError{kInvalidParams, "param must be one of the nvarguscamerasrc ISP "
                   "properties (see PROTOCOL.md)"});
    }

    if (params == nullptr || !json_object_has_member(params, "value")) {
        return camera::base::makeUnexpected(ControlError{kInvalidParams, "missing value"});
    }
    JsonNode* vn = json_object_get_member(params, "value");
    if (JSON_NODE_HOLDS_NULL(vn)) {  // forget the override
        auto source = create_source(cam.source);
        if (source)
            source->set_isp(*cam_idx, cam, name, "", ctx.stream);
        XLOGF(INFO, "control: cam%d isp %s reset", *cam_idx, name.c_str());
        return empty_result();
    }
    if (!JSON_NODE_HOLDS_VALUE(vn)) {
        return camera::base::makeUnexpected(ControlError{kInvalidParams, "value must be a string, number, bool or null"});
    }

    std::string value;
    const GType t = json_node_get_value_type(vn);
    if (t == G_TYPE_STRING) {
        value = json_node_get_string(vn);
    } else if (t == G_TYPE_BOOLEAN) {
        value = json_node_get_boolean(vn) ? "true" : "false";
    } else if (t == G_TYPE_INT64) {
        value = std::to_string(json_node_get_int(vn));
    } else {
        char buf[32];
        g_snprintf(buf, sizeof(buf), "%g", json_node_get_double(vn));
        value = buf;
    }

    auto source = create_source(cam.source);
    if (!source) {
        return camera::base::makeUnexpected(ControlError{kFailed, "unknown source '" + cam.source + "'"});
    }
    SourceResult r = source->set_isp(*cam_idx, cam, name, value, ctx.stream);
    if (!r) {
        return camera::base::makeUnexpected(ControlError{kFailed, std::move(r.error())});
    }
    XLOGF(INFO, "control: cam%d isp %s = %s", *cam_idx, name.c_str(),
              value.c_str());
    return empty_result();
}

}  // namespace camera
