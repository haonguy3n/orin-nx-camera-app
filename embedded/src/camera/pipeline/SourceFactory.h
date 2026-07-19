// Camera source creation.
#pragma once

#include <memory>
#include <string>

#include "camera/pipeline/CameraSource.h"

namespace camera {

// Creates the source strategy for |source_type| ("argus", "v4l2", "test").
// Returns nullptr for unknown types.
std::unique_ptr<ICameraSource> create_source(const std::string& source_type);

}  // namespace camera
