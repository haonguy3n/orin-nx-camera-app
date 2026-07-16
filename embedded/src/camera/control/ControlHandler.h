// Control handler interface (Command pattern) and context.
//
// Each control protocol method (proto/PROTOCOL.md) is implemented as a
// separate IControlHandler class. The ControlRegistry maps method names
// to handler instances, replacing the original 370-line dispatch() if-else
// chain. This satisfies OCP (new method = new handler class + register
// call, no existing code modified) and SRP (each handler has one reason
// to change).
#pragma once

#include <json-glib/json-glib.h>

#include <functional>
#include <string>

#include "camera/config/Config.h"
#include "camera/core/StreamController.h"
#include "camera/pipeline/CameraSource.h"
#include "camera/pipeline/SourceFactory.h"
#include "camera/update/SwupdateClient.h"
#include "camera/lib/v4l2/V4l2Device.h"
#include "camera/folly/Expected.h"

namespace camera {

// Bundles all the dependencies a control handler needs. References must
// outlive the handler call (they point to Application-owned state).
struct ControlContext {
    Config& config;
    IStreamController& stream;
    IV4l2DeviceFactory& v4l2_factory;
    ISourceFactory& source_factory;
    SwupdateClient& swupdate;
    std::function<void()> reload;
};

// A failed control call: JSON-RPC style code + human-readable message.
struct ControlError {
    int code;
    std::string message;
};

// Result of a control call: the result node (transfer full) or an error.
using HandlerResult = folly::Expected<JsonNode*, ControlError>;

// Interface for one control protocol method handler.
class IControlHandler {
public:
    virtual ~IControlHandler() = default;

    // The method name this handler responds to (e.g. "get-status").
    virtual std::string method() const = 0;

    // Handles the request.
    virtual HandlerResult handle(JsonObject* params, ControlContext& ctx) = 0;
};

}  // namespace camera
