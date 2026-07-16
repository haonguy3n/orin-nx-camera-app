#include "camera/control/handlers/UpdateHandler.h"

#include "camera/control/JsonUtil.h"

namespace camera {

namespace {

const char* state_name(UpdateState s) {
    switch (s) {
    case UpdateState::Idle:       return proto::update_states::kIdle;
    case UpdateState::Uploading:  return proto::update_states::kUploading;
    case UpdateState::Installing: return proto::update_states::kInstalling;
    case UpdateState::Success:    return proto::update_states::kSuccess;
    case UpdateState::Failure:    return proto::update_states::kFailure;
    case UpdateState::Done:       return proto::update_states::kDone;
    }
    return "unknown";
}

}  // namespace

HandlerResult GetUpdateStatusHandler::handle(JsonObject* /*params*/, ControlContext& ctx) {
    UpdateStatus s = ctx.swupdate.get_status();

    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "state");
    json_builder_add_string_value(b, state_name(s.state));
    json_builder_set_member_name(b, "percent");
    json_builder_add_int_value(b, s.percent);
    json_builder_set_member_name(b, "step");
    json_builder_add_int_value(b, s.step);
    json_builder_set_member_name(b, "total_steps");
    json_builder_add_int_value(b, s.total_steps);
    if (!s.current_name.empty()) {
        json_builder_set_member_name(b, "current");
        json_builder_add_string_value(b, s.current_name.c_str());
    }
    if (!s.error.empty()) {
        json_builder_set_member_name(b, "error");
        json_builder_add_string_value(b, s.error.c_str());
    }
    json_builder_end_object(b);
    return take_root(b);
}

}  // namespace camera
