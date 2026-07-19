#include "camera/pipeline/PipelineBuilder.h"

#include <glib.h>

namespace camera {

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

std::string PipelineBuilder::appsink_tail(const CameraConfig& cam) {
    // Same encode chain as nvenc_tail, terminated at the parser instead of
    // the RTP payloader: the secure USB transport wants the H.265 elementary
    // stream, not RTP. Tapping here is what lets a USB-only device skip RTSP
    // entirely -- no payload/depayload round trip through loopback.
    const bool h265 = cam.codec == "h265";
    std::string s = "queue ! ";
    s += h265 ? "nvv4l2h265enc" : "nvv4l2h264enc";
    s += " bitrate=" + std::to_string(cam.bitrate) +
         " insert-sps-pps=true idrinterval=30 maxperf-enable=true ! ";
    s += h265 ? "h265parse" : "h264parse";
    s += " config-interval=-1 ! ";
    s += h265 ? "video/x-h265,stream-format=byte-stream"
              : "video/x-h264,stream-format=byte-stream";
    s += " ! appsink name=sink sync=false max-buffers=8 drop=true";
    return s;
}

std::string PipelineBuilder::detect_branch(int width, int height) {
    // NVMM -> CPU BGRx via nvvidconv, straight into the appsink. Named
    // "detect" so Pipeline can find it.
    //
    // leaky=downstream on this queue is essential, not cosmetic: without it a
    // slow detector (YuNet warmup, inference) fills the queue and backpressures
    // THROUGH THE TEE into the encode branch, stalling the video the host
    // actually watches. Leaky drops old frames on this branch so detection is
    // best-effort and can never block video. max-size-buffers=1 also keeps
    // Argus NVMM buffers from being held here.
    // nvvidconv does the resize as well as the NVMM->CPU download. It has to:
    // a plain colour converter cannot rescale, so width/height must ride on
    // nvvidconv's output caps.
    //
    // async=false is what makes the leaky queue above safe. A sink normally
    // gates the pipeline's async state change until it prerolls one buffer --
    // but leaky=downstream drops exactly that buffer, so this appsink never
    // prerolls and the WHOLE pipeline stays stuck in PAUSED: no video on the
    // encode branch either, and teardown of a stuck pipeline is what makes
    // restarts crawl. async=false takes this sink out of the state change, so
    // the detect branch can stall or drop freely without touching video.
    // No videoconvert: it is NOT in the device image (only the nv* GStreamer
    // plugin packages are installed), and gst_parse_launch answers a missing
    // element with a PARTIAL pipeline plus a non-fatal error -- so this whole
    // branch was being dropped while "sink" survived. Video streamed perfectly
    // and detection silently never ran. nvvidconv emits system-memory BGRx by
    // itself, so the branch ends at the appsink and the detector drops the
    // padding byte (cheaper than a CPU colour conversion anyway).
    return "queue leaky=downstream max-size-buffers=1"
           " ! nvvidconv ! video/x-raw,format=BGRx"
           ",width=" + std::to_string(width) +
           ",height=" + std::to_string(height) +
           " ! appsink name=detect sync=false async=false"
           " max-buffers=1 drop=true";
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

}  // namespace camera
