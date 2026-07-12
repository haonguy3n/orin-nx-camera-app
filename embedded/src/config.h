// camera-streamer configuration (GKeyFile INI).
#pragma once

#include <map>
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

    // Sensor settings, also settable at runtime via the control protocol
    // (proto/PROTOCOL.md). argus: mapped to nvarguscamerasrc range
    // properties; v4l2: mapped to the VC driver's V4L2 controls.
    int exposure = 0;               // µs; 0 = auto (argus) / driver default
    double gain = 0;                // 0 = auto/default; argus: multiplier,
                                    // v4l2: raw units (VC: milli-dB, 0-48000)
    int trigger = -1;               // VC trigger mode 0..7; -1 = leave as is

    // Runtime ISP overrides (argus only): nvarguscamerasrc property ->
    // value, appended to the launch string and settable live via set-isp.
    // Config file: isp-<property>= keys, e.g. isp-wbmode=1.
    std::map<std::string, std::string> isp;

    // Digital zoom: GPU crop+upscale via nvvidconv. 1.0 = full FoV (no
    // converter in the pipeline); zoom_x/zoom_y = crop center (0..1).
    double zoom = 1.0;
    double zoom_x = 0.5;
    double zoom_y = 0.5;
};

struct Config {
    static constexpr int kNumCameras = 2;

    int port = 8554;
    // Where to serve RTSP:
    //   all      - every interface (USB and ethernet at once, 0.0.0.0)
    //   usb      - the USB gadget network only (interface usb0)
    //   ethernet - the wired network only (interface eth0)
    //   <other>  - an explicit IPv4 address or interface name
    // Switch at runtime by editing the file and sending SIGHUP
    // (systemctl reload camera-streamer).
    std::string listen = "all";
    // RTP transport(s) the RTSP server offers: tcp (interleaved in the
    // RTSP connection, default — survives hosts that drop unsolicited
    // inbound UDP), udp, or all (client picks; gst clients prefer UDP).
    std::string transport = "tcp";
    // TCP control server (proto/PROTOCOL.md), bound to the same address as
    // the RTSP server. 0 disables it.
    int control_port = 8555;
    // UDP discovery responder (always 0.0.0.0). 0 disables it.
    int discovery_port = 8556;
    CameraConfig cameras[kNumCameras];
};

// Loads the INI file at |path|. A missing file yields built-in defaults
// (cam0/cam1 enabled, argus sensor-id 0/1, h265 @ 8 Mbit/s); a malformed
// file or invalid values fall back to defaults with a warning.
Config load_config(const std::string& path);
