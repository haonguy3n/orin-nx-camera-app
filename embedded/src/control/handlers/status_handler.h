// Status handler: get-status and get-config.
#pragma once

#include "control/control_handler.h"

class GetStatusHandler : public IControlHandler {
public:
    std::string method() const override { return "get-status"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};

class GetConfigHandler : public IControlHandler {
public:
    std::string method() const override { return "get-config"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};
