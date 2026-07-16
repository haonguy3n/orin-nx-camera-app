// System handlers: ping, reload, and reboot.
#pragma once

#include "camera/control/ControlHandler.h"
#include "proto/Protocol.h"

namespace camera {

class PingHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kPing; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

class ReloadHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kReload; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

class RebootHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kReboot; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

}  // namespace camera
