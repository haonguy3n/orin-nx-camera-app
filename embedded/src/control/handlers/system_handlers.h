// System handlers: ping, reload, and reboot.
#pragma once

#include "control/control_handler.h"

class PingHandler : public IControlHandler {
public:
    std::string method() const override { return "ping"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};

class ReloadHandler : public IControlHandler {
public:
    std::string method() const override { return "reload"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};

class RebootHandler : public IControlHandler {
public:
    std::string method() const override { return "reboot"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};
