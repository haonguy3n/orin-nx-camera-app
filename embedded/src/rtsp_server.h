// RTSP server exposing one mount per enabled camera (/cam0, /cam1).
#pragma once

#include <gst/rtsp-server/rtsp-server.h>

#include "config.h"

class RtspServer {
public:
    explicit RtspServer(const Config& config);
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    // Creates the media factories and attaches the server to the default
    // GMainContext. Returns false if no camera is enabled or attach fails.
    bool start();

private:
    const Config& config_;
    GstRTSPServer* server_ = nullptr;
    guint attach_id_ = 0;
};
