#include "camera/control/handlers/RegisterHandlers.h"

#include "camera/control/handlers/ExposureHandler.h"
#include "camera/control/handlers/IspHandler.h"
#include "camera/control/handlers/StatusHandler.h"
#include "camera/control/handlers/SystemHandlers.h"
#include "camera/control/handlers/TriggerHandler.h"
#include "camera/control/handlers/UpdateHandler.h"
#include "camera/control/handlers/V4l2ControlHandler.h"
#include "camera/control/handlers/ZoomHandler.h"

namespace camera {

void register_all_handlers(ControlRegistry& registry) {
    registry.register_handler(std::make_unique<PingHandler>());
    registry.register_handler(std::make_unique<ReloadHandler>());
    registry.register_handler(std::make_unique<RebootHandler>());
    registry.register_handler(std::make_unique<GetStatusHandler>());
    registry.register_handler(std::make_unique<GetConfigHandler>());
    registry.register_handler(std::make_unique<SetExposureHandler>());
    registry.register_handler(std::make_unique<SetGainHandler>());
    registry.register_handler(std::make_unique<SetTriggerHandler>());
    registry.register_handler(std::make_unique<FireTriggerHandler>());
    registry.register_handler(std::make_unique<SetSyncHandler>());
    registry.register_handler(std::make_unique<SetIspHandler>());
    registry.register_handler(std::make_unique<SetZoomHandler>());
    registry.register_handler(std::make_unique<ListControlsHandler>());
    registry.register_handler(std::make_unique<GetControlHandler>());
    registry.register_handler(std::make_unique<SetControlHandler>());
    registry.register_handler(std::make_unique<GetUpdateStatusHandler>());
}

}  // namespace camera
