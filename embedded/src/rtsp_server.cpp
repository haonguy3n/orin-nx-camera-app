#include "rtsp_server.h"

#include <string>

#include "net_util.h"
#include "v4l2_ctrl.h"

namespace {

// Stall watchdog: a live PLAYING pipeline that pushes no buffer through its
// payloader for kStallChecks consecutive periods is declared dead.
constexpr int kWatchdogPeriodSec = 5;
constexpr int kStallChecks = 3;

// nvarguscamerasrc range properties pinning exposure/gain to a fixed value
// (min == max leaves the 3A loop no freedom). Empty when auto.
std::string argus_ranges(const CameraConfig& cam) {
    std::string s;
    if (cam.exposure > 0) {
        const std::string ns = std::to_string(static_cast<long long>(cam.exposure) * 1000);
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

// Encoder + parser + payloader tail shared by the argus and v4l2 pipelines.
// nvv4l2h26xenc bitrate is in bit/s. The queues are load-bearing, not
// cosmetic: inside gst-rtsp-server the whole chain shares one streaming
// thread, and without a queue in front of the encoder the NVMM buffer pool
// starves after the first frame (source blocks waiting for a free buffer
// while the encoder blocks on downstream) — observed as a one-packet
// stream and a stalled pipeline on target.
std::string nvenc_tail(const CameraConfig& cam) {
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

std::string caps_tail(const CameraConfig& cam) {
    return "width=" + std::to_string(cam.width) +
           ",height=" + std::to_string(cam.height) +
           ",framerate=" + std::to_string(cam.framerate) + "/1";
}

// gst_parse_launch pipeline for one camera, wrapped in ( ) as required by
// gst_rtsp_media_factory_set_launch. The source is always named "camsrc" so
// the control server can reach the live element (runtime exposure/gain).
std::string build_launch(const CameraConfig& cam) {
    std::string p = "( ";
    if (cam.source == "argus") {
        p += "nvarguscamerasrc name=camsrc sensor-id=" +
             std::to_string(cam.sensor_id) + argus_ranges(cam) +
             " ! video/x-raw(memory:NVMM)," + caps_tail(cam) + " ! " +
             nvenc_tail(cam);
    } else if (cam.source == "v4l2") {
        // GRAY8 explicitly: the mono IMX296's native 10-bit grey (Y10) has
        // no v4l2src mapping, so an unconstrained video/x-raw fails caps
        // negotiation (mount answers 503). The VC driver also serves 8-bit
        // grey, which v4l2src and nvvidconv both handle; nvvidconv converts
        // into NVMM NV12 for the encoder.
        p += "v4l2src name=camsrc device=" + cam.device +
             " ! video/x-raw,format=GRAY8," + caps_tail(cam) +
             " ! nvvidconv ! video/x-raw(memory:NVMM),format=NV12 ! " +
             nvenc_tail(cam);
    } else {  // test: software pipeline, runs on any host (CI / development).
        const bool h265 = cam.codec == "h265";
        p += "videotestsrc name=camsrc is-live=true ! video/x-raw," +
             caps_tail(cam) + " ! videoconvert ! queue ! ";
        // x26xenc bitrate is in kbit/s.
        p += std::string(h265 ? "x265enc" : "x264enc") +
             " tune=zerolatency key-int-max=30 bitrate=" +
             std::to_string(cam.bitrate / 1000) + " ! ";
        p += h265 ? "h265parse" : "h264parse";
        p += " ! queue ! ";
        p += h265 ? "rtph265pay" : "rtph264pay";
        p += " name=pay0 pt=96";
    }
    p += " )";
    return p;
}

// Config sensor settings for the v4l2 path go straight to the VC driver's
// V4L2 controls (no pipeline needed). Failures are warnings: the device may
// legitimately lack a control, or not exist on a development host.
void apply_v4l2_settings(const CameraConfig& cam, int index) {
    std::string err;
    if (cam.exposure > 0 &&
        !v4l2_set_control(cam.device, "exposure", cam.exposure, &err))
        g_warning("cam%d: %s", index, err.c_str());
    if (cam.gain > 0 &&
        !v4l2_set_control(cam.device, "gain",
                          static_cast<int64_t>(cam.gain), &err))
        g_warning("cam%d: %s", index, err.c_str());
    if (cam.trigger >= 0 && !v4l2_set_trigger_mode(cam.device, cam.trigger, &err))
        g_warning("cam%d: %s", index, err.c_str());
}

const char* client_ip(GstRTSPClient* client) {
    GstRTSPConnection* conn = gst_rtsp_client_get_connection(client);
    const char* ip = conn ? gst_rtsp_connection_get_ip(conn) : nullptr;
    return ip ? ip : "unknown";
}

}  // namespace

GstPadProbeReturn RtspServer::on_payload_buffer(GstPad* /*pad*/,
                                                GstPadProbeInfo* info,
                                                gpointer user_data) {
    auto* cam = static_cast<CamState*>(user_data);
    const guint64 count =
        cam->frames.fetch_add(1, std::memory_order_relaxed) + 1;

    // The payloader pushes a GstBufferList when a frame spans several RTP
    // packets — the norm for 1080p H.265 — and a BUFFER-only probe never
    // sees those (the counter read 0 on target while the stream played).
    // One push = one frame either way; take metadata from the first packet.
    GstBuffer* buffer;
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
        GstBufferList* list = GST_PAD_PROBE_INFO_BUFFER_LIST(info);
        buffer = gst_buffer_list_length(list) > 0
                     ? gst_buffer_list_get(list, 0)
                     : nullptr;
        if (buffer == nullptr)
            return GST_PAD_PROBE_OK;
    } else {
        buffer = GST_PAD_PROBE_INFO_BUFFER(info);
    }
    // v4l2src puts the capture sequence in the buffer offset; sources that
    // don't (argus, videotestsrc pipelines after encoding) fall back to the
    // running frame count so `sequence` is always usable.
    guint64 sequence = GST_BUFFER_OFFSET(buffer);
    if (sequence == GST_BUFFER_OFFSET_NONE)
        sequence = count - 1;
    cam->last_sequence.store(sequence, std::memory_order_relaxed);
    if (GST_BUFFER_PTS_IS_VALID(buffer))
        cam->last_pts.store(GST_BUFFER_PTS(buffer), std::memory_order_relaxed);
    cam->last_wallclock.store(g_get_real_time(), std::memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

RtspServer::RtspServer(const Config& config) : config_(config) {
    for (int i = 0; i < Config::kNumCameras; ++i) {
        cams_[i].self = this;
        cams_[i].index = i;
        g_weak_ref_init(&cams_[i].media, nullptr);
        g_weak_ref_init(&cams_[i].source, nullptr);
    }
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
    for (auto& cam : cams_) {
        if (cam.factory != nullptr)
            g_object_unref(cam.factory);
        g_weak_ref_clear(&cam.media);
        g_weak_ref_clear(&cam.source);
    }
}

// Tracks the live pipeline of a (shared) mount: weak refs to the media and
// its source element, and a frame counter on the payloader for get-status
// and the stall watchdog.
void RtspServer::on_media_configure(GstRTSPMediaFactory* /*factory*/,
                                    GstRTSPMedia* media, gpointer user_data) {
    auto* cam = static_cast<CamState*>(user_data);

    GstElement* bin = gst_rtsp_media_get_element(media);
    GstElement* src = gst_bin_get_by_name(GST_BIN(bin), "camsrc");
    GstElement* pay = gst_bin_get_by_name(GST_BIN(bin), "pay0");

    g_weak_ref_set(&cam->media, media);
    g_weak_ref_set(&cam->source, src);
    if (pay != nullptr) {
        GstPad* pad = gst_element_get_static_pad(pay, "src");
        if (pad != nullptr) {
            gst_pad_add_probe(
                pad,
                static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER |
                                             GST_PAD_PROBE_TYPE_BUFFER_LIST),
                on_payload_buffer, cam, nullptr);
            gst_object_unref(pad);
        }
        gst_object_unref(pay);
    }
    if (src != nullptr)
        gst_object_unref(src);
    gst_object_unref(bin);

    g_signal_connect(media, "unprepared", G_CALLBACK(on_media_unprepared), cam);
    // src/pay presence diagnoses why runtime controls / frame counters
    // would not engage (both should always be found).
    g_message("/cam%d: pipeline created (camsrc %s, pay0 %s)", cam->index,
              src != nullptr ? "found" : "MISSING",
              pay != nullptr ? "probed" : "MISSING");
}

void RtspServer::on_media_unprepared(GstRTSPMedia* /*media*/,
                                     gpointer user_data) {
    auto* cam = static_cast<CamState*>(user_data);
    g_weak_ref_set(&cam->media, nullptr);
    g_weak_ref_set(&cam->source, nullptr);
    g_message("/cam%d: pipeline stopped", cam->index);
}

gboolean RtspServer::on_watchdog(gpointer user_data) {
    auto* self = static_cast<RtspServer*>(user_data);
    for (auto& cam : self->cams_) {
        if (!cam.mounted)
            continue;
        const guint64 frames = cam.frames.load(std::memory_order_relaxed);

        auto* media = static_cast<GstRTSPMedia*>(g_weak_ref_get(&cam.media));
        if (media == nullptr) {
            cam.stalled_checks = 0;
            cam.last_frames = frames;
            continue;
        }
        // Only a PLAYING pipeline is expected to produce frames — prepared
        // (DESCRIBE'd) or paused media is not a stall.
        GstElement* bin = gst_rtsp_media_get_element(media);
        GstState state = GST_STATE_NULL;
        gst_element_get_state(bin, &state, nullptr, 0);
        gst_object_unref(bin);
        g_object_unref(media);

        if (state != GST_STATE_PLAYING || frames != cam.last_frames) {
            cam.stalled_checks = 0;
            cam.last_frames = frames;
            continue;
        }
        if (++cam.stalled_checks >= kStallChecks) {
            g_critical("watchdog: /cam%d is live but produced no frame for "
                       "%d s — exiting for a clean restart",
                       cam.index, kStallChecks * kWatchdogPeriodSec);
            if (self->stall_handler_)
                self->stall_handler_();
            return G_SOURCE_CONTINUE;  // the handler quits the main loop
        }
        g_warning("watchdog: /cam%d produced no frame in the last %d s (%d/%d)",
                  cam.index, kWatchdogPeriodSec, cam.stalled_checks,
                  kStallChecks);
    }
    return G_SOURCE_CONTINUE;
}

bool RtspServer::start() {
    address_ = resolve_listen(config_.listen);
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

        const std::string mount = "/cam" + std::to_string(i);
        const std::string launch = build_launch(cam);

        GstRTSPMediaFactory* factory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_launch(factory, launch.c_str());
        // Offered RTP transports. TCP-interleaved by default: hosts with
        // stateful inbound-UDP filtering silently lose UDP RTP (server
        // sends, client never receives), and clients auto-negotiate TCP
        // when UDP isn't offered.
        GstRTSPLowerTrans trans = GST_RTSP_LOWER_TRANS_TCP;
        if (config_.transport == "udp")
            trans = static_cast<GstRTSPLowerTrans>(GST_RTSP_LOWER_TRANS_UDP |
                                                   GST_RTSP_LOWER_TRANS_UDP_MCAST);
        else if (config_.transport == "all")
            trans = static_cast<GstRTSPLowerTrans>(GST_RTSP_LOWER_TRANS_UDP |
                                                   GST_RTSP_LOWER_TRANS_UDP_MCAST |
                                                   GST_RTSP_LOWER_TRANS_TCP);
        gst_rtsp_media_factory_set_protocols(factory, trans);
        // One pipeline per mount regardless of client count, so multiple
        // clients don't re-open the sensor.
        gst_rtsp_media_factory_set_shared(factory, TRUE);
        g_signal_connect(factory, "media-configure",
                         G_CALLBACK(on_media_configure), &cams_[i]);
        gst_rtsp_mount_points_add_factory(mounts, mount.c_str(), factory);
        cams_[i].factory =
            static_cast<GstRTSPMediaFactory*>(g_object_ref(factory));
        cams_[i].mounted = true;

        if (cam.source == "v4l2")
            apply_v4l2_settings(cam, i);

        g_message("mount %s: %s", mount.c_str(), launch.c_str());
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
    watchdog_id_ =
        g_timeout_add_seconds(kWatchdogPeriodSec, on_watchdog, this);

    g_message("RTSP server listening on rtsp://%s:%d (listen=%s, %d stream%s)",
              address_.c_str(), config_.port, config_.listen.c_str(), enabled,
              enabled == 1 ? "" : "s");
    return true;
}

StreamStatus RtspServer::stream_status(int cam) {
    StreamStatus s;
    s.mounted = cams_[cam].mounted;
    s.frames = cams_[cam].frames.load(std::memory_order_relaxed);
    s.sequence = cams_[cam].last_sequence.load(std::memory_order_relaxed);
    s.pts = cams_[cam].last_pts.load(std::memory_order_relaxed);
    s.wallclock = cams_[cam].last_wallclock.load(std::memory_order_relaxed);
    if (auto* media = g_weak_ref_get(&cams_[cam].media)) {
        s.streaming = true;
        g_object_unref(media);
    }
    return s;
}

void RtspServer::refresh_launch(int cam) {
    if (cams_[cam].factory == nullptr)
        return;
    const std::string launch = build_launch(config_.cameras[cam]);
    gst_rtsp_media_factory_set_launch(cams_[cam].factory, launch.c_str());
    g_message("/cam%d: launch refreshed: %s", cam, launch.c_str());
}

bool RtspServer::set_source_property(int cam, const char* property,
                                     const char* value) {
    auto* src = static_cast<GstElement*>(g_weak_ref_get(&cams_[cam].source));
    if (src == nullptr)
        return false;
    gst_util_set_object_arg(G_OBJECT(src), property, value);
    gst_object_unref(src);
    return true;
}
