#include "control/handlers/zoom_handler.h"

#include <glib.h>

#include "control/json_util.h"

JsonNode* SetZoomHandler::handle(JsonObject* params, ControlContext& ctx,
                                 int* err_code, std::string* err_msg) {
    int cam_idx;
    if (!param_camera(params, &cam_idx)) {
        *err_code = kInvalidParams;
        *err_msg = "camera must be 0 or 1";
        return nullptr;
    }
    double factor;
    if (!param_double(params, "factor", &factor) || factor < 1.0 ||
        factor > 8.0) {
        *err_code = kInvalidParams;
        *err_msg = "factor must be a number in 1.0-8.0";
        return nullptr;
    }

    CameraConfig& cam = ctx.config.cameras[cam_idx];
    double v;
    if (param_double(params, "x", &v))
        cam.zoom_x = CLAMP(v, 0.0, 1.0);
    if (param_double(params, "y", &v))
        cam.zoom_y = CLAMP(v, 0.0, 1.0);
    cam.zoom = factor;
    // The re-armed launch string is authoritative (clients reconnect to
    // pick it up); there is no reliable live crop update on nvvidconv.
    ctx.stream.refresh_launch(cam_idx);
    g_message("control: cam%d zoom = %.2fx @ (%.2f, %.2f)", cam_idx,
              cam.zoom, cam.zoom_x, cam.zoom_y);
    return empty_result();
}
