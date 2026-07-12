#include "control/handlers/register_handlers.h"

#include "control/handlers/exposure_handler.h"
#include "control/handlers/isp_handler.h"
#include "control/handlers/status_handler.h"
#include "control/handlers/system_handlers.h"
#include "control/handlers/trigger_handler.h"
#include "control/handlers/v4l2_control_handler.h"
#include "control/handlers/zoom_handler.h"

void register_all_handlers(ControlRegistry& registry) {
    registry.register_handler(std::make_unique<PingHandler>());
    registry.register_handler(std::make_unique<ReloadHandler>());
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
}
