// Zoom handler: set-zoom (digital zoom + pan).
#pragma once

#include "control/control_handler.h"

class SetZoomHandler : public IControlHandler {
public:
    std::string method() const override { return "set-zoom"; }
    JsonNode* handle(JsonObject* params, ControlContext& ctx,
                     int* err_code, std::string* err_msg) override;
};
