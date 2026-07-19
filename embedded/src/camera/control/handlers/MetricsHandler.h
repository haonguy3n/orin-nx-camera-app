// Metrics handler: get-metrics (CPU/GPU/thermal), for measuring a change.
#pragma once

#include "camera/control/ControlHandler.h"
#include "proto/Protocol.h"

namespace camera {

class GetMetricsHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kGetMetrics; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

}  // namespace camera
