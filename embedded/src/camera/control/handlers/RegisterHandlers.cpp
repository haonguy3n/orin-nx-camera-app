#include "camera/control/handlers/RegisterHandlers.h"

#include "camera/control/handlers/ExposureHandler.h"
#include "camera/control/handlers/IspHandler.h"
#include "camera/control/handlers/StatusHandler.h"
#if ENABLE_SECURE_USB
#include "camera/control/handlers/SnapshotHandler.h"
#endif
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
    registry.register_handler(std::make_unique<SetStreamHandler>());
    registry.register_handler(std::make_unique<ListControlsHandler>());
    registry.register_handler(std::make_unique<GetControlHandler>());
    registry.register_handler(std::make_unique<SetControlHandler>());
    registry.register_handler(std::make_unique<GetUpdateStatusHandler>());
#if ENABLE_SECURE_USB
    // Snapshot reads from the detection branch, which the secure USB
    // pipeline owns; without that transport there is no frame to grab.
    registry.register_handler(std::make_unique<SnapshotHandler>());
#endif
}

}  // namespace camera
