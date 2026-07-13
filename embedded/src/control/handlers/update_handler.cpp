#include "control/handlers/update_handler.h"

#include "control/json_util.h"

namespace {

const char* state_name(UpdateState s) {
    switch (s) {
    case UpdateState::Idle:       return "idle";
    case UpdateState::Uploading:  return "uploading";
    case UpdateState::Installing: return "installing";
    case UpdateState::Success:    return "success";
    case UpdateState::Failure:    return "failure";
    case UpdateState::Done:       return "done";
    }
    return "unknown";
}

}  // namespace

JsonNode* GetUpdateStatusHandler::handle(JsonObject* /*params*/,
                                         ControlContext& ctx,
                                         int* /*err_code*/,
                                         std::string* /*err_msg*/) {
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
