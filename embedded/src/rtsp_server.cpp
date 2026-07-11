#include "rtsp_server.h"

#include <string>

namespace {

// Encoder + parser + payloader tail shared by the argus and v4l2 pipelines.
// nvv4l2h26xenc bitrate is in bit/s.
std::string nvenc_tail(const CameraConfig& cam) {
    const bool h265 = cam.codec == "h265";
    std::string s = h265 ? "nvv4l2h265enc" : "nvv4l2h264enc";
    s += " bitrate=" + std::to_string(cam.bitrate) +
         " insert-sps-pps=true idrinterval=30 ! ";
    s += h265 ? "h265parse ! rtph265pay" : "h264parse ! rtph264pay";
    s += " name=pay0 pt=96";
    return s;
}

std::string caps_tail(const CameraConfig& cam) {
    return "width=" + std::to_string(cam.width) +
           ",height=" + std::to_string(cam.height) +
           ",framerate=" + std::to_string(cam.framerate) + "/1";
}

// gst_parse_launch pipeline for one camera, wrapped in ( ) as required by
// gst_rtsp_media_factory_set_launch.
std::string build_launch(const CameraConfig& cam) {
    std::string p = "( ";
    if (cam.source == "argus") {
        p += "nvarguscamerasrc sensor-id=" + std::to_string(cam.sensor_id) +
             " ! video/x-raw(memory:NVMM)," + caps_tail(cam) + " ! " +
             nvenc_tail(cam);
    } else if (cam.source == "v4l2") {
        // Best-effort for M1: let v4l2src/nvvidconv negotiate the raw format,
        // convert into NVMM for the HW encoder.
        p += "v4l2src device=" + cam.device + " ! video/x-raw," + caps_tail(cam) +
             " ! nvvidconv ! video/x-raw(memory:NVMM) ! " + nvenc_tail(cam);
    } else {  // test: software pipeline, runs on any host (CI / development).
        const bool h265 = cam.codec == "h265";
        p += "videotestsrc is-live=true ! video/x-raw," + caps_tail(cam) +
             " ! videoconvert ! ";
        // x26xenc bitrate is in kbit/s.
        p += std::string(h265 ? "x265enc" : "x264enc") +
             " tune=zerolatency key-int-max=30 bitrate=" +
             std::to_string(cam.bitrate / 1000) + " ! ";
        p += h265 ? "h265parse ! rtph265pay" : "h264parse ! rtph264pay";
        p += " name=pay0 pt=96";
    }
    p += " )";
    return p;
}

void on_client_closed(GstRTSPClient* client, gpointer /*user_data*/) {
    GstRTSPConnection* conn = gst_rtsp_client_get_connection(client);
    g_message("client disconnected: %s",
              conn ? gst_rtsp_connection_get_ip(conn) : "unknown");
}

void on_client_connected(GstRTSPServer* /*server*/, GstRTSPClient* client,
                         gpointer /*user_data*/) {
    GstRTSPConnection* conn = gst_rtsp_client_get_connection(client);
    g_message("client connected: %s",
              conn ? gst_rtsp_connection_get_ip(conn) : "unknown");
    g_signal_connect(client, "closed", G_CALLBACK(on_client_closed), nullptr);
}

}  // namespace

RtspServer::RtspServer(const Config& config) : config_(config) {}

RtspServer::~RtspServer() {
    if (attach_id_ != 0)
        g_source_remove(attach_id_);
    if (server_ != nullptr)
        g_object_unref(server_);
}

bool RtspServer::start() {
    server_ = gst_rtsp_server_new();
    gst_rtsp_server_set_address(server_, "0.0.0.0");
    gst_rtsp_server_set_service(server_, std::to_string(config_.port).c_str());

    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server_);
    int enabled = 0;
    for (int i = 0; i < Config::kNumCameras; ++i) {
        const CameraConfig& cam = config_.cameras[i];
        if (!cam.enabled)
            continue;

        const std::string mount = "/cam" + std::to_string(i);
        const std::string launch = build_launch(cam);

        GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_launch(factory, launch.c_str());
        // One pipeline per mount regardless of client count, so multiple
        // clients don't re-open the sensor.
        gst_rtsp_media_factory_set_shared(factory, TRUE);
        gst_rtsp_mount_points_add_factory(mounts, mount.c_str(), factory);

        g_message("mount %s: %s", mount.c_str(), launch.c_str());
        ++enabled;
    }
    g_object_unref(mounts);

    if (enabled == 0) {
        g_printerr("no cameras enabled, nothing to serve\n");
        return false;
    }

    g_signal_connect(server_, "client-connected",
                     G_CALLBACK(on_client_connected), nullptr);

    attach_id_ = gst_rtsp_server_attach(server_, nullptr);
    if (attach_id_ == 0) {
        g_printerr("failed to attach RTSP server on port %d\n", config_.port);
        return false;
    }

    g_message("RTSP server listening on rtsp://0.0.0.0:%d (%d stream%s)",
              config_.port, enabled, enabled == 1 ? "" : "s");
    return true;
}
