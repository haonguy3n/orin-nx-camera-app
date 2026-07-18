// RTSP server exposing one mount per enabled camera (/cam0, /cam1).
//
// Implements IStreamController so control handlers and source strategies
// can manipulate the live pipeline without depending on this concrete
// class. Delegates per-camera state to MountController instances.
#pragma once

#include <gst/rtsp-server/rtsp-server.h>

#include <array>
#include <functional>
#include <memory>
#include <string>

#include "camera/config/Config.h"
#include "camera/core/StreamController.h"
#include "camera/pipeline/CameraSource.h"
#include "camera/pipeline/SourceFactory.h"
#include "camera/rtsp/MountController.h"
#include "camera/base/Expected.h"
#include "camera/base/Unit.h"

namespace camera {

class RtspServer : public IStreamController {
public:
    explicit RtspServer(const Config& config, ISourceFactory& source_factory);
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    // Creates the media factories and attaches the server to the default
    // GMainContext. Returns false if no camera is enabled or attach fails.
    camera::base::Expected<camera::base::Unit, std::string> start();

    // IStreamController
    const std::string& bound_address() const override { return address_; }
    int client_count() const override { return clients_; }
    StreamStatus stream_status(int cam) override;
    bool set_source_property(int cam, const char* property,
                             const char* value) override;
    void refresh_launch(int cam) override;

    // Called (from the main loop) when ALL cameras have stopped producing
    // frames; the service exits so systemd restarts it cleanly. A single
    // stalled camera is disabled in place and the rest keep streaming.
    void set_stall_handler(std::function<void()> handler) {
        stall_handler_ = std::move(handler);
    }

private:
    static gboolean on_watchdog(gpointer user_data);
    void disable_mount(int cam);

    const Config& config_;
    ISourceFactory& source_factory_;
    std::string address_;
    GstRTSPServer* server_ = nullptr;
    guint attach_id_ = 0;
    guint watchdog_id_ = 0;
    int clients_ = 0;
    std::array<std::unique_ptr<MountController>, Config::kNumCameras> mounts_;
    std::function<void()> stall_handler_;
};

}  // namespace camera
