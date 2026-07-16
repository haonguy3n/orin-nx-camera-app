// V4L2 control handler: list-controls, get-control, set-control.
// Generic escape hatch for every V4L2 control the VC driver exposes.
#pragma once

#include "camera/control/ControlHandler.h"
#include "proto/Protocol.h"

namespace camera {

class ListControlsHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kListControls; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

class GetControlHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kGetControl; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

class SetControlHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kSetControl; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

}  // namespace camera
