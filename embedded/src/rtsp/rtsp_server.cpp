#include "rtsp/rtsp_server.h"

#include <glib.h>

#include <string>

#include "net/network_resolver.h"

namespace {

const char* client_ip(GstRTSPClient* client) {
    GstRTSPConnection* conn = gst_rtsp_client_get_connection(client);
    const char* ip = conn ? gst_rtsp_connection_get_ip(conn) : nullptr;
    return ip ? ip : "unknown";
}

}  // namespace

RtspServer::RtspServer(const Config& config, ISourceFactory& source_factory)
    : config_(config), source_factory_(source_factory) {
    for (int i = 0; i < Config::kNumCameras; ++i)
        mounts_[i] = std::make_unique<MountController>(
            i, "/cam" + std::to_string(i));
}

// Full teardown so a new server can bind the same port right away (runtime
// interface switch): drop the listening socket, close every client (which
// tears down its sessions and media), and drop any remaining sessions.
RtspServer::~RtspServer() {
    if (watchdog_id_ != 0)
        g_source_remove(watchdog_id_);
    if (server_ != nullptr) {
        if (attach_id_ != 0)
            g_source_remove(attach_id_);
        // The filters return the (empty, nothing REF'd) list of matches.
        g_list_free(gst_rtsp_server_client_filter(
            server_,
            [](GstRTSPServer*, GstRTSPClient*, gpointer) {
                return GST_RTSP_FILTER_REMOVE;
            },
            nullptr));
        GstRTSPSessionPool* pool = gst_rtsp_server_get_session_pool(server_);
        g_list_free(gst_rtsp_session_pool_filter(
            pool,
            [](GstRTSPSessionPool*, GstRTSPSession*, gpointer) {
                return GST_RTSP_FILTER_REMOVE;
            },
            nullptr));
        g_object_unref(pool);
        g_object_unref(server_);
    }
    // mounts_ unique_ptrs clean up themselves.
}

bool RtspServer::start() {
    address_ = NetworkResolver::resolve_listen(config_.listen);
    if (address_.empty()) {
        g_printerr("listen=%s: no usable IPv4 address (interface down or "
                   "no address yet?)\n", config_.listen.c_str());
        return false;
    }

    server_ = gst_rtsp_server_new();
    gst_rtsp_server_set_address(server_, address_.c_str());
    gst_rtsp_server_set_service(server_, std::to_string(config_.port).c_str());

    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server_);
    int enabled = 0;
    for (int i = 0; i < Config::kNumCameras; ++i) {
        const CameraConfig& cam = config_.cameras[i];
        if (!cam.enabled)
            continue;

        auto source = source_factory_.create(cam.source);
        if (!source) {
            g_warning("cam%d: unknown source '%s', skipping", i,
                      cam.source.c_str());
            continue;
        }

        const std::string launch = source->build_launch(cam);

        // Offered RTP transports. TCP-interleaved by default: hosts with
        // stateful inbound-UDP filtering silently lose UDP RTP (server
        // sends, client never receives), and clients auto-negotiate TCP
        // when UDP isn't offered.
        GstRTSPLowerTrans trans = GST_RTSP_LOWER_TRANS_TCP;
        if (config_.transport == "udp")
            trans = static_cast<GstRTSPLowerTrans>(
                GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_UDP_MCAST);
        else if (config_.transport == "all")
            trans = static_cast<GstRTSPLowerTrans>(
                GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_UDP_MCAST |
                GST_RTSP_LOWER_TRANS_TCP);

        mounts_[i]->install(mounts, launch, trans);

        // Apply initial V4L2 settings (v4l2 source only).
        source->apply_initial_settings(cam);

        g_message("mount %s: %s", mounts_[i]->mount_path().c_str(),
                  launch.c_str());
        ++enabled;
    }
    g_object_unref(mounts);

    if (enabled == 0) {
        g_printerr("no cameras enabled, nothing to serve\n");
        return false;
    }

    g_signal_connect(
        server_, "client-connected",
        G_CALLBACK(+[](GstRTSPServer*, GstRTSPClient* client, gpointer data) {
            auto* self = static_cast<RtspServer*>(data);
            ++self->clients_;
            g_message("client connected: %s", client_ip(client));
            g_signal_connect(client, "closed",
                             G_CALLBACK(+[](GstRTSPClient* c, gpointer d) {
                                 --static_cast<RtspServer*>(d)->clients_;
                                 g_message("client disconnected: %s",
                                           client_ip(c));
                             }),
                             data);
        }),
        this);

    attach_id_ = gst_rtsp_server_attach(server_, nullptr);
    if (attach_id_ == 0) {
        g_printerr("failed to attach RTSP server on %s:%d\n", address_.c_str(),
                   config_.port);
        return false;
    }
    watchdog_id_ = g_timeout_add_seconds(kWatchdogPeriodSec, on_watchdog, this);

    g_message("RTSP server listening on rtsp://%s:%d (listen=%s, %d stream%s)",
              address_.c_str(), config_.port, config_.listen.c_str(), enabled,
              enabled == 1 ? "" : "s");
    return true;
}

gboolean RtspServer::on_watchdog(gpointer user_data) {
    auto* self = static_cast<RtspServer*>(user_data);
    for (auto& mount : self->mounts_) {
        if (!mount->mounted())
            continue;
        const double want =
            self->config_.cameras[mount->index()].framerate;
        if (mount->check_stall(want)) {
            if (self->stall_handler_)
                self->stall_handler_();
            return G_SOURCE_CONTINUE;  // the handler quits the main loop
        }
    }
    return G_SOURCE_CONTINUE;
}

StreamStatus RtspServer::stream_status(int cam) {
    return mounts_[cam]->status();
}

void RtspServer::refresh_launch(int cam) {
    auto source = source_factory_.create(config_.cameras[cam].source);
    if (!source)
        return;
    const std::string launch = source->build_launch(config_.cameras[cam]);
    mounts_[cam]->refresh_launch(launch);
}

bool RtspServer::set_source_property(int cam, const char* property,
                                     const char* value) {
    return mounts_[cam]->set_source_property(property, value);
}
