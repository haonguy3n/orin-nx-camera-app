// camera-streamer configuration (GKeyFile INI).
#pragma once

#include <string>

// One camera = one RTSP mount (/cam0, /cam1).
struct CameraConfig {
    bool enabled = true;
    std::string source = "argus";   // argus | v4l2 | test
    int sensor_id = 0;              // argus: nvarguscamerasrc sensor-id
    std::string device;             // v4l2: /dev/videoN (default derived from index)
    int width = 1440;
    int height = 1080;
    int framerate = 60;
    std::string codec = "h265";     // h265 | h264
    int bitrate = 8000000;          // bit/s
};

struct Config {
    static constexpr int kNumCameras = 2;

    int port = 8554;
    CameraConfig cameras[kNumCameras];
};

// Loads the INI file at |path|. A missing file yields built-in defaults
// (cam0/cam1 enabled, argus sensor-id 0/1, h265 @ 8 Mbit/s); a malformed
// file or invalid values fall back to defaults with a warning.
Config load_config(const std::string& path);
