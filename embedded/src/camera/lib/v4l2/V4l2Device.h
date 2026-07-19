// V4L2 device control access.
//
// V4l2Device wraps the direct ioctl calls to /dev/videoN. It holds only the
// device path; every operation opens the node, acts, and closes it, so an
// instance is cheap to construct wherever one is needed.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "camera/base/Expected.h"
#include "camera/base/File.h"
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

// One V4L2 device node (/dev/videoN). All operations return
// camera::base::Expected: the value on success, an error message on failure
// (device missing, not a V4L2 device, unknown control, ...).
class V4l2Device {
public:
    explicit V4l2Device(std::string device_path)
        : device_path_(std::move(device_path)) {}

    // Every non-disabled scalar control the device exposes.
    camera::base::Expected<std::vector<V4l2Control>, std::string>
    list_controls();

    // |control| is a control name (matched case-insensitively, with space,
    // '_' and '-' treated as equal -- "trigger_mode" matches "Trigger Mode")
    // or a numeric id (decimal or 0x-prefixed hex).
    camera::base::Expected<V4l2Control, std::string> get_control(
        const std::string& control);
    camera::base::Expected<camera::base::Unit, std::string> set_control(
        const std::string& control, int64_t value);

    // Sets the VC MIPI hardware trigger mode (0 = disabled .. 7 = stream
    // level). Finds the driver's control by name ("trigger_mode" on the VC
    // driver; falls back to the first non-button control containing
    // "trigger").
    camera::base::Expected<camera::base::Unit, std::string> set_trigger_mode(
        int mode);

    // Presses the VC driver's software "single trigger" button control
    // (exposes one frame when the sensor is in a software-triggerable mode).
    camera::base::Expected<camera::base::Unit, std::string>
    fire_single_trigger();

private:
    camera::base::Expected<camera::base::File, std::string> open_device() const;

    std::string device_path_;
};

}  // namespace camera
