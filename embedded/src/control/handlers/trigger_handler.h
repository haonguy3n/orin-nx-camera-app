// Trigger handler: set-trigger, fire-trigger, and set-sync.
#pragma once

#include "control/control_handler.h"

class SetTriggerHandler : public IControlHandler {
public:
    std::string method() const override { return "set-trigger"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};

class FireTriggerHandler : public IControlHandler {
public:
    std::string method() const override { return "fire-trigger"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};

class SetSyncHandler : public IControlHandler {
public:
    std::string method() const override { return "set-sync"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};
