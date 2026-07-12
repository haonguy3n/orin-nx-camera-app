#include "control/handlers/isp_handler.h"

#include <glib.h>

#include <string>

#include "control/json_util.h"

namespace {

// nvarguscamerasrc properties settable via set-isp (proto/PROTOCOL.md).
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

JsonNode* SetIspHandler::handle(JsonObject* params, ControlContext& ctx,
                                int* err_code, std::string* err_msg) {
    int cam_idx;
    if (!param_camera(params, &cam_idx)) {
        *err_code = kInvalidParams;
        *err_msg = "camera must be 0 or 1";
        return nullptr;
    }
    CameraConfig& cam = ctx.config.cameras[cam_idx];
    if (cam.source != "argus") {
        *err_code = kFailed;
        *err_msg = "ISP controls require the argus source "
                   "(current source '" + cam.source + "')";
        return nullptr;
    }

    std::string name;
    if (params != nullptr && json_object_has_member(params, "param")) {
        JsonNode* n = json_object_get_member(params, "param");
        if (JSON_NODE_HOLDS_VALUE(n) &&
            json_node_get_value_type(n) == G_TYPE_STRING)
            name = json_node_get_string(n);
    }
    if (!is_isp_param(name)) {
        *err_code = kInvalidParams;
        *err_msg = "param must be one of the nvarguscamerasrc ISP "
                   "properties (see PROTOCOL.md)";
        return nullptr;
    }

    if (params == nullptr || !json_object_has_member(params, "value")) {
        *err_code = kInvalidParams;
        *err_msg = "missing value";
        return nullptr;
    }
    JsonNode* vn = json_object_get_member(params, "value");
    if (JSON_NODE_HOLDS_NULL(vn)) {  // forget the override
        auto source = ctx.source_factory.create(cam.source);
        if (source)
            source->set_isp(cam_idx, cam, name, "", ctx.stream);
        g_message("control: cam%d isp %s reset", cam_idx, name.c_str());
        return empty_result();
    }
    if (!JSON_NODE_HOLDS_VALUE(vn)) {
        *err_code = kInvalidParams;
        *err_msg = "value must be a string, number, bool or null";
        return nullptr;
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

    auto source = ctx.source_factory.create(cam.source);
    if (!source) {
        *err_code = kFailed;
        *err_msg = "unknown source '" + cam.source + "'";
        return nullptr;
    }
    SourceResult r = source->set_isp(cam_idx, cam, name, value, ctx.stream);
    if (!r.ok) {
        *err_code = kFailed;
        *err_msg = r.error;
        return nullptr;
    }
    g_message("control: cam%d isp %s = %s", cam_idx, name.c_str(),
              value.c_str());
    return empty_result();
}
