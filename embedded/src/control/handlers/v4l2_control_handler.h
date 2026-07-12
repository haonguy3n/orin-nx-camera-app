// V4L2 control handler: list-controls, get-control, set-control.
// Generic escape hatch for every V4L2 control the VC driver exposes.
#pragma once

#include "control/control_handler.h"

class ListControlsHandler : public IControlHandler {
public:
    std::string method() const override { return "list-controls"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};

class GetControlHandler : public IControlHandler {
public:
    std::string method() const override { return "get-control"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};

class SetControlHandler : public IControlHandler {
public:
    std::string method() const override { return "set-control"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};
