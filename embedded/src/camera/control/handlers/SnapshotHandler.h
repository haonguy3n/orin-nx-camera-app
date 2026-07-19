// Snapshot handler: snapshot (write one frame to disk, for ISP debugging).
#pragma once

#include "camera/control/ControlHandler.h"
#include "proto/Protocol.h"

namespace camera {

class SnapshotHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kSnapshot; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

}  // namespace camera
