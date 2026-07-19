#include "camera/rtsp/RtspServer.h"

#include <unistd.h>

#include <algorithm>

#include "camera/pipeline/PipelineBuilder.h"

#include <glib.h>

#include <string>

#include "camera/lib/net/NetworkResolver.h"

#include "camera/base/logging/xlog.h"

namespace camera {

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

camera::base::Expected<camera::base::Unit, std::string> RtspServer::start() {
    address_ = NetworkResolver::resolve_listen(config_.listen);
    if (address_.empty()) {
        return camera::base::makeUnexpected(
            "listen=" + config_.listen +
            ": no usable IPv4 address (interface down or no address yet?)");
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
            XLOGF(WARN, "cam%d: unknown source '%s', skipping", i,
                      cam.source.c_str());
            continue;
        }

        std::string launch = source->build_launch(cam);
        // Face detection in network mode: tee a detect branch off alongside
        // the RTP payloader. gst-rtsp-server only requires that "pay0" exists,
        // so the appsink can live in the same bin; MountController picks it up
        // on media-configure. Same swap trick the USB launch uses, so the
        // source builders stay unaware of detection.
        if (meta_sink_ != nullptr && !config_.detect_model.empty() &&
            access(config_.detect_model.c_str(), R_OK) == 0) {
            const std::string plain = PipelineBuilder::nvenc_tail(cam);
            const size_t at = launch.rfind(plain);
            if (at != std::string::npos) {
                const int detect_h =
                    cam.width > 0
                        ? std::max(2, static_cast<int>(
                                          static_cast<long long>(
                                              config_.detect_width) *
                                          cam.height / cam.width) & ~1)
                        : config_.detect_width;
                launch.replace(at, plain.size(),
                               PipelineBuilder::nvenc_tail_with_detect(
                                   cam, config_.detect_width, detect_h));
                mounts_[i]->enable_detection(meta_sink_, config_.detect_model,
                                             config_.detect_width, detect_h,
                                             config_.detect_score,
                                             config_.detect_fps);
                XLOGF(INFO, "rtsp: cam%d detection enabled (%dx%d, score>=%.2f,"
                            " %d fps)", i, config_.detect_width, detect_h,
                      config_.detect_score, config_.detect_fps);
            }
        }

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

        XLOGF(INFO, "mount %s: %s", mounts_[i]->mount_path().c_str(),
                  launch.c_str());
        ++enabled;
    }
    g_object_unref(mounts);

    if (enabled == 0) {
        return camera::base::makeUnexpected(
            std::string("no cameras enabled, nothing to serve"));
    }

    g_signal_connect(
        server_, "client-connected",
        G_CALLBACK(+[](GstRTSPServer*, GstRTSPClient* client, gpointer data) {
            auto* self = static_cast<RtspServer*>(data);
            ++self->clients_;
            XLOGF(INFO, "client connected: %s", client_ip(client));
            g_signal_connect(client, "closed",
                             G_CALLBACK(+[](GstRTSPClient* c, gpointer d) {
                                 --static_cast<RtspServer*>(d)->clients_;
                                 XLOGF(INFO, "client disconnected: %s",
                                           client_ip(c));
                             }),
                             data);
        }),
        this);

    attach_id_ = gst_rtsp_server_attach(server_, nullptr);
    if (attach_id_ == 0) {
        return camera::base::makeUnexpected("failed to attach RTSP server on " +
                                     address_ + ":" +
                                     std::to_string(config_.port));
    }
    watchdog_id_ = g_timeout_add_seconds(kWatchdogPeriodSec, on_watchdog, this);

    XLOGF(INFO, "RTSP server listening on rtsp://%s:%d (listen=%s, %d stream%s)",
              address_.c_str(), config_.port, config_.listen.c_str(), enabled,
              enabled == 1 ? "" : "s");
    return camera::base::unit;
}

gboolean RtspServer::on_watchdog(gpointer user_data) {
    auto* self = static_cast<RtspServer*>(user_data);
    // One dead camera must not take down the healthy one(s): disable the
    // stalled mount and keep serving the rest. Only when nothing is left
    // does the stall handler fire (systemd restart -- nothing to preserve).
    bool any_mounted = false;
    for (auto& mount : self->mounts_) {
        if (!mount->mounted())
            continue;
        const double want =
            self->config_.cameras[mount->index()].framerate;
        if (mount->check_stall(want))
            self->disable_mount(mount->index());
        else
            any_mounted = true;
    }
    if (!any_mounted && self->stall_handler_)
        self->stall_handler_();
    return G_SOURCE_CONTINUE;
}

void RtspServer::disable_mount(int cam) {
    // Unmount so new clients get 404 instead of a frozen stream. Existing
    // clients of this mount keep a silent connection until their own
    // timeout; the wedged pipeline is left untouched (teardown of a
    // stalled pipeline SIGBUSed on target).
    GstRTSPMountPoints* mounts = gst_rtsp_server_get_mount_points(server_);
    gst_rtsp_mount_points_remove_factory(
        mounts, mounts_[cam]->mount_path().c_str());
    g_object_unref(mounts);
    mounts_[cam]->disable();
    XLOGF(ERR, "%s: camera dead, mount disabled -- other streams keep "
               "running (reload or restart to re-enable)",
               mounts_[cam]->mount_path().c_str());
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

}  // namespace camera
