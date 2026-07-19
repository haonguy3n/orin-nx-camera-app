// An IStreamController that also drives the secure-USB pipeline.
//
// Runtime settings (set-exposure, set-gain, set-zoom) reach the hardware
// through IStreamController, whose only implementation is RtspServer -- it
// pokes the source element inside the RTSP pipeline. With transports=usb that
// pipeline is never instantiated, so those calls silently did nothing: the
// frames come from SecureUsbServer's own pipeline.
//
// Worse, SecureUsbServer's launch strings were built once at startup, so a
// setting did not even reach the NEXT session. It stayed dead until restart.
//
// This wraps the RTSP controller and fans the mutating calls out to both, so
// the handlers and the wire protocol need no knowledge of the transport.
#pragma once

#include <functional>
#include <memory>
#include <string>

#include "camera/core/StreamController.h"
#include "camera/secure/SecureUsbServer.h"

namespace camera {

class UsbAwareStreamController : public IStreamController {
public:
    // `usb` is read through a getter because the secure server is constructed
    // after the control server; `build_launch` regenerates one camera's USB
    // launch description from the (already mutated) config.
    // `rtsp` may be null: under transports=usb no RTSP server is constructed
    // at all, and the USB pipeline is the only thing to drive.
    UsbAwareStreamController(IStreamController* rtsp,
                             std::function<secure::SecureUsbServer*()> usb,
                             std::function<std::string(int)> build_launch)
        : rtsp_(rtsp), usb_(std::move(usb)),
          build_launch_(std::move(build_launch)) {}

    // Status still comes from the RTSP side. Reporting the USB transport's
    // liveness through get-status is a separate change -- under transports=usb
    // these read as a permanently idle server.
    const std::string& bound_address() const override {
        static const std::string kNone;
        return rtsp_ != nullptr ? rtsp_->bound_address() : kNone;
    }
    int client_count() const override {
        return rtsp_ != nullptr ? rtsp_->client_count() : 0;
    }
    StreamStatus stream_status(int cam) override {
        // Without RTSP this reports an empty status rather than a wrong one.
        // Sourcing it from the USB transport's own frame counters is the
        // remaining piece -- see the get-status gap.
        return rtsp_ != nullptr ? rtsp_->stream_status(cam) : StreamStatus{};
    }

    // True if either transport took it. The RTSP side is asked first so
    // transports=both keeps its existing behaviour exactly.
    bool set_source_property(int cam, const char* property,
                             const char* value) override {
        const bool on_rtsp =
            rtsp_ != nullptr && rtsp_->set_source_property(cam, property, value);
        bool on_usb = false;
        if (auto* server = usb_ ? usb_() : nullptr)
            on_usb = server->set_source_property(static_cast<uint8_t>(cam),
                                                 property, value);
        return on_rtsp || on_usb;
    }

    void refresh_launch(int cam) override {
        if (rtsp_ != nullptr) rtsp_->refresh_launch(cam);
        auto* server = usb_ ? usb_() : nullptr;
        if (server == nullptr || !build_launch_) return;
        std::string launch = build_launch_(cam);
        if (!launch.empty())
            server->refresh_launch(static_cast<uint8_t>(cam), std::move(launch));
    }

private:
    IStreamController* rtsp_;
    std::function<secure::SecureUsbServer*()> usb_;
    std::function<std::string(int)> build_launch_;
};

}  // namespace camera
