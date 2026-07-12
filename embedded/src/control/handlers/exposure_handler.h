// Exposure handler: set-exposure and set-gain.
#pragma once

#include "control/control_handler.h"

class SetExposureHandler : public IControlHandler {
public:
    std::string method() const override { return "set-exposure"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};

class SetGainHandler : public IControlHandler {
public:
    std::string method() const override { return "set-gain"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};
