#include "pipeline/pipeline_builder.h"

#include <glib.h>

std::string PipelineBuilder::argus_ranges(const CameraConfig& cam) {
    std::string s;
    if (cam.exposure > 0) {
        const std::string ns =
            std::to_string(static_cast<long long>(cam.exposure) * 1000);
        s += " exposuretimerange=\"" + ns + " " + ns + "\"";
    }
    if (cam.gain > 0) {
        char buf[32];
        g_snprintf(buf, sizeof(buf), "%g", cam.gain);
        s += " gainrange=\"" + std::string(buf) + " " + buf + "\"";
    }
    // ISP overrides (isp-* config keys / set-isp): always quoted, which
    // gst_parse_launch accepts for every property type.
    for (const auto& [property, value] : cam.isp)
        s += " " + property + "=\"" + value + "\"";
    return s;
}

std::string PipelineBuilder::nvenc_tail(const CameraConfig& cam) {
    const bool h265 = cam.codec == "h265";
    std::string s = "queue ! ";
    s += h265 ? "nvv4l2h265enc" : "nvv4l2h264enc";
    s += " bitrate=" + std::to_string(cam.bitrate) +
         " insert-sps-pps=true idrinterval=30 maxperf-enable=true ! ";
    s += h265 ? "h265parse" : "h264parse";
    s += " ! queue ! ";
    s += h265 ? "rtph265pay" : "rtph264pay";
    s += " name=pay0 pt=96";
    return s;
}

std::string PipelineBuilder::zoom_crop(const CameraConfig& cam) {
    if (cam.zoom <= 1.0)
        return "";
    const int cw = MAX(2, static_cast<int>(cam.width / cam.zoom) & ~1);
    const int ch = MAX(2, static_cast<int>(cam.height / cam.zoom) & ~1);
    int left = static_cast<int>(cam.zoom_x * cam.width - cw / 2.0) & ~1;
    int top = static_cast<int>(cam.zoom_y * cam.height - ch / 2.0) & ~1;
    left = CLAMP(left, 0, cam.width - cw);
    top = CLAMP(top, 0, cam.height - ch);
    return " left=" + std::to_string(left) +
           " right=" + std::to_string(left + cw) +
           " top=" + std::to_string(top) +
           " bottom=" + std::to_string(top + ch);
}

std::string PipelineBuilder::zoom_tail(const CameraConfig& cam) {
    return " ! nvvidconv name=zoomer" + zoom_crop(cam) +
           " ! video/x-raw(memory:NVMM),format=NV12,width=" +
           std::to_string(cam.width) + ",height=" +
           std::to_string(cam.height);
}

std::string PipelineBuilder::caps_tail(const CameraConfig& cam) {
    return "width=" + std::to_string(cam.width) +
           ",height=" + std::to_string(cam.height) +
           ",framerate=" + std::to_string(cam.framerate) + "/1";
}
