#include "pipeline/v4l2_source.h"

#include <glib.h>

#include <string>

#include "core/stream_controller.h"
#include "pipeline/pipeline_builder.h"

std::string V4l2Source::build_launch(const CameraConfig& cam) const {
    // GRAY8 explicitly: the mono IMX296's native 10-bit grey (Y10) has
    // no v4l2src mapping, so an unconstrained video/x-raw fails caps
    // negotiation (mount answers 503). The VC driver also serves 8-bit
    // grey, which v4l2src and nvvidconv both handle; nvvidconv converts
    // into NVMM NV12 for the encoder (and crops when zoomed).
    std::string p = "( ";
    p += "v4l2src name=camsrc device=" + cam.device +
         " ! video/x-raw,format=GRAY8," + PipelineBuilder::caps_tail(cam) +
         PipelineBuilder::zoom_tail(cam) + " ! " +
         PipelineBuilder::nvenc_tail(cam);
    p += " )";
    return p;
}

void V4l2Source::apply_initial_settings(const CameraConfig& cam) const {
    // Config sensor settings for the v4l2 path go straight to the VC
    // driver's V4L2 controls (no pipeline needed). Failures are warnings:
    // the device may legitimately lack a control, or not exist on a
    // development host.
    auto dev = v4l2_factory_.open(cam.device);
    if (!dev)
        return;
    std::string err;
    if (cam.exposure > 0 &&
        !dev->set_control("exposure", cam.exposure, &err))
        g_warning("v4l2: %s", err.c_str());
    if (cam.gain > 0 &&
        !dev->set_control("gain", static_cast<int64_t>(cam.gain), &err))
        g_warning("v4l2: %s", err.c_str());
    if (cam.trigger >= 0 && !dev->set_trigger_mode(cam.trigger, &err))
        g_warning("v4l2: %s", err.c_str());
}

SourceResult V4l2Source::set_exposure(int /*cam_index*/, CameraConfig& cam,
                                      int us,
                                      IStreamController& /*stream*/) const {
    auto dev = v4l2_factory_.open(cam.device);
    if (!dev)
        return {false, cam.device + ": cannot open device"};
    std::string err;
    if (us == 0) {  // 0 = back to the driver default
        V4l2Control c;
        if (!dev->get_control("exposure", &c, &err))
            return {false, err};
        if (!dev->set_control("exposure", c.default_value, &err))
            return {false, err};
    } else {
        if (!dev->set_control("exposure", us, &err))
            return {false, err};
    }
    cam.exposure = us;
    return {};
}

SourceResult V4l2Source::set_gain(int /*cam_index*/, CameraConfig& cam,
                                  double gain,
                                  IStreamController& /*stream*/) const {
    auto dev = v4l2_factory_.open(cam.device);
    if (!dev)
        return {false, cam.device + ": cannot open device"};
    std::string err;
    if (gain == 0) {  // 0 = back to the driver default
        V4l2Control c;
        if (!dev->get_control("gain", &c, &err))
            return {false, err};
        if (!dev->set_control("gain", c.default_value, &err))
            return {false, err};
    } else {
        if (!dev->set_control("gain", static_cast<int64_t>(gain), &err))
            return {false, err};
    }
    cam.gain = gain;
    return {};
}

SourceResult V4l2Source::set_trigger(CameraConfig& cam, int mode,
                                     IV4l2DeviceFactory& v4l2) const {
    auto dev = v4l2.open(cam.device);
    if (!dev)
        return {false, cam.device + ": cannot open device"};
    std::string err;
    if (!dev->set_trigger_mode(mode, &err))
        return {false, err};
    cam.trigger = mode;
    return {};
}

SourceResult V4l2Source::set_isp(int /*cam_index*/, CameraConfig& /*cam*/,
                                 const std::string& /*param*/,
                                 const std::string& /*value*/,
                                 IStreamController& /*stream*/) const {
    return {false, "ISP controls require the argus source "
                   "(current source 'v4l2')"};
}
