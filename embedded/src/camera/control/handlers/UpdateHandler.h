// Update handler: get-update-status.
// Reports current SWUpdate installation status/progress to the host UI.
#pragma once

#include "camera/control/ControlHandler.h"
#include "proto/Protocol.h"

namespace camera {

class GetUpdateStatusHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kGetUpdateStatus; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

}  // namespace camera
