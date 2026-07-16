// ISP handler: set-isp (runtime nvarguscamerasrc ISP property override).
#pragma once

#include "camera/control/ControlHandler.h"
#include "proto/Protocol.h"

namespace camera {

class SetIspHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kSetIsp; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

}  // namespace camera
