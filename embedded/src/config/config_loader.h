// Configuration loading abstraction.
//
// IConfigLoader decouples the source of configuration (file, defaults,
// future: command-line, network) from the Config DTO. The GKeyFile-based
// INI loader is the production implementation; tests can inject a mock
// that returns a hand-crafted Config.
#pragma once

#include "config/config.h"

// Interface for configuration loading.
class IConfigLoader {
public:
    virtual ~IConfigLoader() = default;
    // Loads configuration from the implementation-specific source.
    // A missing source yields built-in defaults; a malformed source
    // falls back to defaults with warnings.
    virtual Config load() = 0;
};

// Production loader: reads a GKeyFile INI file from |path|.
// A missing file yields built-in defaults (cam0/cam1 enabled, argus
// sensor-id 0/1, h265 @ 8 Mbit/s); a malformed file or invalid values
// fall back to defaults with a warning.
class FileConfigLoader : public IConfigLoader {
public:
    explicit FileConfigLoader(std::string path);

    Config load() override;

private:
    std::string path_;
};
