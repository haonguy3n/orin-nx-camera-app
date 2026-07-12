// Control method registry (Registry pattern).
//
// Maps method names to IControlHandler instances. The ControlServer looks
// up the handler by method name and delegates. Adding a new method is a
// matter of creating a handler and registering it -- no existing code
// changes needed (OCP).
#pragma once

#include <memory>
#include <string>
#include <unordered_map>

#include "control/control_handler.h"

class ControlRegistry {
public:
    // Registers a handler for its method name. Takes ownership.
    void register_handler(std::unique_ptr<IControlHandler> handler);

    // Returns the handler for |method|, or nullptr if unknown.
    IControlHandler* find(const std::string& method) const;

private:
    std::unordered_map<std::string, std::unique_ptr<IControlHandler>> handlers_;
};
