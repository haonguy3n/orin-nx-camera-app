// Registers all control protocol handlers into a ControlRegistry.
// Called once at startup; adding a new method is a matter of creating a
// handler class and adding one register call here.
#pragma once

#include "camera/control/ControlRegistry.h"

namespace camera {

// Registers all built-in control handlers into |registry|.
void register_all_handlers(ControlRegistry& registry);

}  // namespace camera
