// Update handler: get-update-status.
// Reports current SWUpdate installation status/progress to the host UI.
#pragma once

#include "control/control_handler.h"

class GetUpdateStatusHandler : public IControlHandler {
public:
    std::string method() const override { return "get-update-status"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};
