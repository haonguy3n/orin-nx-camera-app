// Generation of libargus's camera_overrides.isp from TuningConfig, so the
// deep ISP parameters (black level, white trim) become runtime-tunable:
// rewrite the file, restart nvargus-daemon, recreate the pipelines.
#pragma once

#include <string>

#include "config.h"

// Where libargus looks for override tuning. Overridable via the
// CAMERA_STREAMER_ISP_FILE environment variable (host testing).
const char* isp_file_path();

// The full overrides-file content for |tuning|.
std::string isp_file_content(const TuningConfig& tuning);

// Writes the file if its content differs. Returns true if it changed
// (i.e. nvargus-daemon must be restarted to pick it up); false when
// already in sync or unwritable (logged).
bool isp_file_sync(const TuningConfig& tuning);
