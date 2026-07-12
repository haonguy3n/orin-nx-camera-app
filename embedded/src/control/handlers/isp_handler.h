// ISP handler: set-isp (runtime nvarguscamerasrc ISP property override).
#pragma once

#include "control/control_handler.h"

class SetIspHandler : public IControlHandler {
public:
    std::string method() const override { return "set-isp"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};
