// Exposure handler: set-exposure and set-gain.
#pragma once

#include "camera/control/ControlHandler.h"
#include "proto/Protocol.h"

namespace camera {

class SetExposureHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kSetExposure; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

class SetGainHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kSetGain; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

}  // namespace camera
