// camera-streamer configuration data structures (DTOs).
//
// These are plain data transfer objects — no behavior, no dependencies.
// Loading is handled by ConfigLoader (config_loader.h); runtime mutation
// is done by the control handlers operating on Config& directly.
#pragma once

#include <map>
#include <string>

#include "proto/Protocol.h"

namespace camera {

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
    int exposure = 0;               // us; 0 = auto (argus) / driver default
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

    int port = proto::kRtspPort;
    // Where to serve RTSP:
    //   all      - every interface (USB and ethernet at once, 0.0.0.0)
    //   usb      - the USB gadget network only (interface usb0)
    //   ethernet - the wired network only (interface eth0)
    //   <other>  - an explicit IPv4 address or interface name
    // Switch at runtime by editing the file and sending SIGHUP
    // (systemctl reload camera-streamer).
    std::string listen = "all";
    // Which transports carry video/control/update:
    //   both     - network and secure USB at once (default)
    //   network  - TCP only; the secure USB endpoint is not published
    //   usb      - secure USB only
    //
    // "usb" serves the cameras straight from the encoder over USB and
    // confines the TCP servers to 127.0.0.1, where the USB transport reaches
    // control and update. The update server is the exception -- see
    // recovery_update below.
    //
    // Distinct from `listen`, which selects *which network* the TCP servers
    // use (its "usb" value means the CDC-NCM gadget network, not this).
    std::string transports = "both";
    // Recovery update channel.
    //
    // With transports=usb everything else is confined to loopback, so a
    // secure USB transport that fails to come up would leave no way to push
    // firmware -- the device would be unrecoverable in the field. When on
    // (the default) the update server additionally binds the CDC-NCM gadget
    // address, which rides the same physical cable, so a broken secure
    // transport is still recoverable. Set off only where the update path is
    // provided some other way.
    bool recovery_update = true;
    // RTP transport(s) the RTSP server offers: tcp (interleaved in the
    // RTSP connection, default -- survives hosts that drop unsolicited
    // inbound UDP), udp, or all (client picks; gst clients prefer UDP).
    std::string transport = "tcp";
    // TCP control server (proto/PROTOCOL.md), bound to the same address as
    // the RTSP server. 0 disables it.
    int control_port = proto::kControlPort;
    // UDP discovery responder (always 0.0.0.0). 0 disables it.
    int discovery_port = proto::kDiscoveryPort;
    // OTA update file upload server (binary .swu upload, same address as
    // RTSP). 0 disables it.
    int update_port = proto::kUpdatePort;
    // TLS for the control and update servers. tls-cert + tls-key set ->
    // both servers speak TLS; tls-ca also set -> clients must present a
    // certificate signed by that CA (mTLS). All unset -> plaintext.
    // Misconfiguration (only one of cert/key, unreadable file) is a fatal
    // startup error, never a silent plaintext fallback.
    std::string tls_cert;  // PEM server certificate path
    std::string tls_key;   // PEM server private key path
    std::string tls_ca;    // PEM CA bundle for client verification

    // On-device face detection (OpenCV YuNet on the GPU). Always on when the
    // model file exists: each secure-USB camera pipeline then grows a raw
    // branch and a detection thread that emits face boxes over the metadata
    // channel, and the host draws the overlay. No separate enable flag -- the
    // model's presence (an image build choice, CAMERA_FACE_MODEL) is the
    // switch. detect_width/height are the detector's working resolution.
    std::string detect_model = "/usr/share/camera-streamer/face_detection_yunet.onnx";
    int detect_width = 320;
    int detect_height = 320;

    CameraConfig cameras[kNumCameras];
};

}  // namespace camera
