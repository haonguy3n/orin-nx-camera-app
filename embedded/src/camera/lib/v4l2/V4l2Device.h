// V4L2 device control access abstraction.
//
// IV4l2Device isolates the rest of the code from direct ioctl calls to
// /dev/videoN. This enables unit-testing handlers that manipulate V4L2
// controls (set-exposure, set-gain, set-trigger, ...) with a mock device.
//
// IV4l2DeviceFactory creates per-device IV4l2Device handles; the factory
// indirection lets tests inject mock devices without touching real hardware.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "camera/base/Expected.h"
#include "camera/base/Unit.h"

namespace camera {

// Describes one V4L2 control exposed by the device.
struct V4l2Control {
    uint32_t id = 0;
    std::string name;
    uint32_t type = 0;      // raw V4L2_CTRL_TYPE_* value
    int64_t minimum = 0;
    int64_t maximum = 0;
    int64_t step = 0;
    int64_t default_value = 0;
    int64_t value = 0;      // current value (0 if write-only/non-scalar)
    uint32_t flags = 0;
};

// Interface for one V4L2 device node (/dev/videoN). All operations
// return camera::base::Expected: the value on success, an error message on
// failure (device missing, not a V4L2 device, unknown control, ...).
class IV4l2Device {
public:
    virtual ~IV4l2Device() = default;

    // Every non-disabled scalar control the device exposes.
    virtual camera::base::Expected<std::vector<V4l2Control>, std::string>
    list_controls() = 0;

    // |control| is a control name (matched case-insensitively, with space,
    // '_' and '-' treated as equal -- "trigger_mode" matches "Trigger Mode")
    // or a numeric id (decimal or 0x-prefixed hex).
    virtual camera::base::Expected<V4l2Control, std::string> get_control(
        const std::string& control) = 0;
    virtual camera::base::Expected<camera::base::Unit, std::string> set_control(
        const std::string& control, int64_t value) = 0;

    // Sets the VC MIPI hardware trigger mode (0 = disabled .. 7 = stream
    // level). Finds the driver's control by name ("trigger_mode" on the VC
    // driver; falls back to the first non-button control containing
    // "trigger").
    virtual camera::base::Expected<camera::base::Unit, std::string> set_trigger_mode(
        int mode) = 0;

    // Presses the VC driver's software "single trigger" button control
    // (exposes one frame when the sensor is in a software-triggerable mode).
    virtual camera::base::Expected<camera::base::Unit, std::string> fire_single_trigger() = 0;
};

// Interface for creating IV4l2Device instances by device path.
class IV4l2DeviceFactory {
public:
    virtual ~IV4l2DeviceFactory() = default;
    // Returns a new IV4l2Device for |device_path| (e.g. "/dev/video0").
    // The caller owns the returned pointer.
    virtual std::unique_ptr<IV4l2Device> open(const std::string& device_path) = 0;
};

}  // namespace camera
