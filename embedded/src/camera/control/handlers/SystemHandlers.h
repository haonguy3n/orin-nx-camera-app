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

// set-stream {camera, enabled}: starts/stops one camera's video without a
// reload. Governs the secure USB push directly (its video loop watches the
// flag); over RTSP a stopped camera refuses new sessions -- reload was the
// only alternative, and a reload arriving over the secure tunnel would tear
// down the very transport carrying the request.
class SetStreamHandler : public IControlHandler {
public:
    std::string method() const override { return proto::methods::kSetStream; }
    HandlerResult handle(JsonObject* params, ControlContext& ctx) override;
};

}  // namespace camera
