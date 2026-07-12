#include "control/handlers/status_handler.h"

#include "control/json_util.h"
#include "v4l2/v4l2_device.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

namespace {

// Shared builder for both get-status and get-config. |include_live| adds
// streaming state, live AE readback, and last_frame metadata.
JsonNode* build_status(const Config& cfg, IStreamController& stream,
                       IV4l2DeviceFactory& v4l2, bool include_live) {
    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "version");
    json_builder_add_string_value(b, APP_VERSION);
    json_builder_set_member_name(b, "listen");
    json_builder_add_string_value(b, cfg.listen.c_str());
    json_builder_set_member_name(b, "port");
    json_builder_add_int_value(b, cfg.port);
    json_builder_set_member_name(b, "control_port");
    json_builder_add_int_value(b, cfg.control_port);
    if (include_live) {
        json_builder_set_member_name(b, "address");
        json_builder_add_string_value(b, stream.bound_address().c_str());
        json_builder_set_member_name(b, "clients");
        json_builder_add_int_value(b, stream.client_count());
    }
    json_builder_set_member_name(b, "cameras");
    json_builder_begin_array(b);
    for (int i = 0; i < Config::kNumCameras; ++i) {
        json_builder_begin_object(b);
        add_camera_config(b, i, cfg.cameras[i]);
        if (include_live) {
            const StreamStatus s = stream.stream_status(i);
            json_builder_set_member_name(b, "streaming");
            json_builder_add_boolean_value(b, s.streaming);
            json_builder_set_member_name(b, "frames");
            json_builder_add_int_value(b, s.frames);
            json_builder_set_member_name(b, "fps");
            json_builder_add_double_value(b, s.fps);
            // Live values programmed into the sensor right now -- when
            // exposure/gain are 0 (auto), this is what Argus AE chose.
            // Read from the driver's V4L2 controls; omitted if the
            // device node can't be queried.
            auto dev = v4l2.open(cfg.cameras[i].device);
            if (dev) {
                V4l2Control live;
                std::string lerr;
                if (dev->get_control("exposure", &live, &lerr)) {
                    json_builder_set_member_name(b, "exposure_current");
                    json_builder_add_int_value(b, live.value);
                }
                if (dev->get_control("gain", &live, &lerr)) {
                    json_builder_set_member_name(b, "gain_current");
                    json_builder_add_int_value(b, live.value);
                }
            }
            if (s.frames > 0) {
                json_builder_set_member_name(b, "last_frame");
                json_builder_begin_object(b);
                json_builder_set_member_name(b, "sequence");
                json_builder_add_int_value(b, s.sequence);
                json_builder_set_member_name(b, "pts");
                json_builder_add_int_value(b, s.pts);
                json_builder_set_member_name(b, "wallclock");
                json_builder_add_int_value(b, s.wallclock);
                json_builder_end_object(b);
            }
        }
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
    json_builder_end_object(b);
    return take_root(b);
}

}  // namespace

JsonNode* GetStatusHandler::handle(JsonObject* /*params*/, ControlContext& ctx,
                                   int* /*err_code*/, std::string* /*err_msg*/) {
    return build_status(ctx.config, ctx.stream, ctx.v4l2_factory, true);
}

JsonNode* GetConfigHandler::handle(JsonObject* /*params*/, ControlContext& ctx,
                                   int* /*err_code*/, std::string* /*err_msg*/) {
    return build_status(ctx.config, ctx.stream, ctx.v4l2_factory, false);
}
