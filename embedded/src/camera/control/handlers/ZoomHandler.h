// Zoom handler: set-zoom (digital zoom + pan).
#pragma once

#include "camera/control/ControlHandler.h"
#include "proto/Protocol.h"

namespace camera {

class SetZoomHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kSetZoom; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

}  // namespace camera
