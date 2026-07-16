// Trigger handler: set-trigger, fire-trigger, and set-sync.
#pragma once

#include "camera/control/ControlHandler.h"
#include "proto/Protocol.h"

namespace camera {

class SetTriggerHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kSetTrigger; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

class FireTriggerHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kFireTrigger; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

class SetSyncHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kSetSync; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

}  // namespace camera
