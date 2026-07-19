#include "camera/control/handlers/SnapshotHandler.h"

#include <glib.h>

#include <unistd.h>

#include <string>

#include "camera/base/logging/xlog.h"
#include "camera/control/JsonUtil.h"
#include "camera/detect/Snapshot.h"

namespace camera {

HandlerResult SnapshotHandler::handle(JsonObject* params, ControlContext& ctx) {
    auto cam_idx = require_camera(params);
    if (!cam_idx) {
        return camera::base::makeUnexpected(cam_idx.error());
    }

    // The frame comes off the detection branch, which only exists when a
    // detection model is present. Say so plainly rather than accepting the
    // request and silently never writing the file.
    if (ctx.config.detect_model.empty() ||
        access(ctx.config.detect_model.c_str(), R_OK) != 0) {
        return camera::base::makeUnexpected(ControlError{
            kFailed, "snapshot needs the detection branch, which is off "
                     "because no readable [detect] model is installed"});
    }

    std::string path = "/tmp/snapshot-cam" + std::to_string(*cam_idx) + ".ppm";
    if (params != nullptr && json_object_has_member(params, "path")) {
        JsonNode* n = json_object_get_member(params, "path");
        if (JSON_NODE_HOLDS_VALUE(n) &&
            json_node_get_value_type(n) == G_TYPE_STRING)
            path = json_node_get_string(n);
    }

    detect::request_snapshot(static_cast<uint8_t>(*cam_idx), path);
    XLOGF(INFO, "control: cam%d snapshot requested -> %s", *cam_idx,
          path.c_str());

    // The write happens on the next detection frame, so the file is not there
    // yet when this returns. Reported as "requested", not "written".
    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "path");
    json_builder_add_string_value(b, path.c_str());
    json_builder_set_member_name(b, "pending");
    json_builder_add_boolean_value(b, TRUE);
    json_builder_end_object(b);
    return take_root(b);
}

}  // namespace camera
