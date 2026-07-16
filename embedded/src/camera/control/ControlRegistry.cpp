#include "camera/control/ControlRegistry.h"

namespace camera {

void ControlRegistry::register_handler(
    std::unique_ptr<IControlHandler> handler) {
    std::string name = handler->method();
    handlers_[std::move(name)] = std::move(handler);
}

IControlHandler* ControlRegistry::find(const std::string& method) const {
    auto it = handlers_.find(method);
    return it != handlers_.end() ? it->second.get() : nullptr;
}

}  // namespace camera
