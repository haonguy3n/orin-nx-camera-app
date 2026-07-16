// GStreamer pipeline launch string construction.
//
// Extracted from the original rtsp_server.cpp's free functions
// (nvenc_tail, zoom_crop, zoom_tail, caps_tail, argus_ranges) into a
// dedicated class. These fragments are shared by all ICameraSource
// strategies, so they live here rather than being duplicated.
#pragma once

#include <string>

#include "camera/config/Config.h"

namespace camera {

class PipelineBuilder {
public:
    // nvarguscamerasrc range properties pinning exposure/gain to a fixed
    // value (min == max leaves the 3A loop no freedom). Empty when auto.
    static std::string argus_ranges(const CameraConfig& cam);

    // Encoder + parser + payloader tail shared by the argus and v4l2
    // pipelines. nvv4l2h26xenc bitrate is in bit/s. The queues are
    // load-bearing, not cosmetic: inside gst-rtsp-server the whole chain
    // shares one streaming thread, and without a queue in front of the
    // encoder the NVMM buffer pool starves after the first frame.
    static std::string nvenc_tail(const CameraConfig& cam);

    // Crop rectangle for digital zoom. nvvidconv's left/right/top/bottom
    // are coordinates of the crop rectangle on its input; even values keep
    // NV12 chroma alignment. Empty at zoom 1 (converter not inserted).
    static std::string zoom_crop(const CameraConfig& cam);

    // Converter stage: crops (when zoomed) and scales back to the full
    // output size on the GPU. The v4l2 path always needs the conversion
    // into NVMM; argus only gets one when zooming.
    static std::string zoom_tail(const CameraConfig& cam);

    // Caps fragment: width=,height=,framerate=
    static std::string caps_tail(const CameraConfig& cam);
};

}  // namespace camera
