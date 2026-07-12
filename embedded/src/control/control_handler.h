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

#include "config/config.h"
#include "core/stream_controller.h"
#include "pipeline/camera_source.h"
#include "pipeline/source_factory.h"
#include "v4l2/v4l2_device.h"

// Bundles all the dependencies a control handler needs. References must
// outlive the handler call (they point to Application-owned state).
struct ControlContext {
    Config& config;
    IStreamController& stream;
    IV4l2DeviceFactory& v4l2_factory;
    ISourceFactory& source_factory;
    std::function<void()> reload;
};

// Interface for one control protocol method handler.
class IControlHandler {
public:
    virtual ~IControlHandler() = default;

    // The method name this handler responds to (e.g. "get-status").
    virtual std::string method() const = 0;

    // Handles the request. Returns the result node (transfer full), or
    // nullptr with |err_code|/|err_msg| set on error.
    virtual JsonNode* handle(JsonObject* params, ControlContext& ctx,
                             int* err_code, std::string* err_msg) = 0;
};
