#include "camera/control/handlers/ZoomHandler.h"

#include <glib.h>

#include "camera/control/JsonUtil.h"

#include "camera/folly/logging/xlog.h"

namespace camera {

HandlerResult SetZoomHandler::handle(JsonObject* params, ControlContext& ctx) {
    auto cam_idx = require_camera(params);
    if (!cam_idx) {
        return folly::makeUnexpected(cam_idx.error());
    }
    double factor;
    if (!param_double(params, "factor", &factor) || factor < 1.0 ||
        factor > 8.0) {
        return folly::makeUnexpected(ControlError{kInvalidParams, "factor must be a number in 1.0-8.0"});
    }

    CameraConfig& cam = ctx.config.cameras[*cam_idx];
    double v;
    if (param_double(params, "x", &v))
        cam.zoom_x = CLAMP(v, 0.0, 1.0);
    if (param_double(params, "y", &v))
        cam.zoom_y = CLAMP(v, 0.0, 1.0);
    cam.zoom = factor;
    // The re-armed launch string is authoritative (clients reconnect to
    // pick it up); there is no reliable live crop update on nvvidconv.
    ctx.stream.refresh_launch(*cam_idx);
    XLOGF(INFO, "control: cam%d zoom = %.2fx @ (%.2f, %.2f)", *cam_idx,
              cam.zoom, cam.zoom_x, cam.zoom_y);
    return empty_result();
}

}  // namespace camera
