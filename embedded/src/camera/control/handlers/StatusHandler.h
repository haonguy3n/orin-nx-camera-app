// Status handler: get-status and get-config.
#pragma once

#include "camera/control/ControlHandler.h"
#include "proto/Protocol.h"

namespace camera {

class GetStatusHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kGetStatus; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

class GetConfigHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kGetConfig; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

}  // namespace camera
