// Configuration loading.
#pragma once

#include "camera/config/Config.h"

namespace camera {

// Reads a GKeyFile INI file from |path|.
// A missing file yields built-in defaults (cam0/cam1 enabled, argus
// sensor-id 0/1, h265 @ 8 Mbit/s); a malformed file or invalid values
// fall back to defaults with a warning.
class FileConfigLoader {
public:
    explicit FileConfigLoader(std::string path);

    Config load();

private:
    std::string path_;
};

}  // namespace camera
