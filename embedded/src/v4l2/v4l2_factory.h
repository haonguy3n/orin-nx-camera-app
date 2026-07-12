// Factory function declaration for the production V4L2 device factory.
// The factory class itself is internal to v4l2_device.cpp; this header
// exposes only the creation function.
#pragma once

#include "v4l2/v4l2_device.h"

#include <memory>

// Creates the production IV4l2DeviceFactory that opens real /dev/videoN
// nodes via ioctl.
std::unique_ptr<IV4l2DeviceFactory> create_v4l2_device_factory();
