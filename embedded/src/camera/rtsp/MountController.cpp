#include "camera/rtsp/MountController.h"

#include <glib.h>

#include "camera/folly/logging/xlog.h"

namespace camera {

MountController::MountController(int index, const std::string& mount_path)
    : index_(index), mount_path_(mount_path) {
    g_weak_ref_init(&media_, nullptr);
    g_weak_ref_init(&source_, nullptr);
}

MountController::~MountController() {
    if (factory_ != nullptr)
        g_object_unref(factory_);
    g_weak_ref_clear(&media_);
    g_weak_ref_clear(&source_);
}

GstPadProbeReturn MountController::on_payload_buffer(GstPad* /*pad*/,
                                                     GstPadProbeInfo* info,
                                                     gpointer user_data) {
    auto* self = static_cast<MountController*>(user_data);

    // The payloader may push one RTP packet per buffer, or a GstBufferList
    // per frame -- packetization varies by codec and frame size, so neither
    // "one push" nor "one buffer" is a frame. Count a distinct PTS instead:
    // every RTP packet of a frame carries that frame's timestamp, so the
    // PTS changes exactly once per frame.
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

    const guint64 pts = GST_BUFFER_PTS_IS_VALID(buffer)
                            ? GST_BUFFER_PTS(buffer)
                            : GST_CLOCK_TIME_NONE;
    if (pts != GST_CLOCK_TIME_NONE &&
        pts == self->last_pts_.load(std::memory_order_relaxed) &&
        self->frames_.load(std::memory_order_relaxed) > 0)
        return GST_PAD_PROBE_OK;  // another packet of the frame just counted

    const guint64 count =
        self->frames_.fetch_add(1, std::memory_order_relaxed) + 1;
    // v4l2src puts the capture sequence in the buffer offset; sources that
    // don't (argus, videotestsrc pipelines after encoding) fall back to the
    // running frame count so `sequence` is always usable.
    guint64 sequence = GST_BUFFER_OFFSET(buffer);
    if (sequence == GST_BUFFER_OFFSET_NONE)
        sequence = count - 1;
    self->last_sequence_.store(sequence, std::memory_order_relaxed);
    if (GST_BUFFER_PTS_IS_VALID(buffer))
        self->last_pts_.store(GST_BUFFER_PTS(buffer), std::memory_order_relaxed);
    self->last_wallclock_.store(g_get_real_time(), std::memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

void MountController::on_media_configure(GstRTSPMediaFactory* /*factory*/,
                                         GstRTSPMedia* media,
                                         gpointer user_data) {
    auto* self = static_cast<MountController*>(user_data);

    GstElement* bin = gst_rtsp_media_get_element(media);
    GstElement* src = gst_bin_get_by_name(GST_BIN(bin), "camsrc");
    GstElement* pay = gst_bin_get_by_name(GST_BIN(bin), "pay0");

    g_weak_ref_set(&self->media_, media);
    g_weak_ref_set(&self->source_, src);
    if (pay != nullptr) {
        GstPad* pad = gst_element_get_static_pad(pay, "src");
        if (pad != nullptr) {
            gst_pad_add_probe(
                pad,
                static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER |
                                             GST_PAD_PROBE_TYPE_BUFFER_LIST),
                on_payload_buffer, self, nullptr);
            gst_object_unref(pad);
        }
        gst_object_unref(pay);
    }
    if (src != nullptr)
        gst_object_unref(src);
    gst_object_unref(bin);

    g_signal_connect(media, "unprepared", G_CALLBACK(on_media_unprepared),
                     self);
    // src/pay presence diagnoses why runtime controls / frame counters
    // would not engage (both should always be found).
    XLOGF(INFO, "%s: pipeline created (camsrc %s, pay0 %s)",
              self->mount_path_.c_str(),
              src != nullptr ? "found" : "MISSING",
              pay != nullptr ? "probed" : "MISSING");
}

void MountController::on_media_unprepared(GstRTSPMedia* /*media*/,
                                          gpointer user_data) {
    auto* self = static_cast<MountController*>(user_data);
    g_weak_ref_set(&self->media_, nullptr);
    g_weak_ref_set(&self->source_, nullptr);
    XLOGF(INFO, "%s: pipeline stopped", self->mount_path_.c_str());
}

void MountController::install(GstRTSPMountPoints* mounts,
                              const std::string& launch,
                              GstRTSPLowerTrans transport) {
    factory_ = gst_rtsp_media_factory_new();
    gst_rtsp_media_factory_set_launch(factory_, launch.c_str());
    gst_rtsp_media_factory_set_protocols(factory_, transport);
    // One pipeline per mount regardless of client count, so multiple
    // clients don't re-open the sensor.
    gst_rtsp_media_factory_set_shared(factory_, TRUE);
    g_signal_connect(factory_, "media-configure",
                     G_CALLBACK(on_media_configure), this);
    gst_rtsp_mount_points_add_factory(mounts, mount_path_.c_str(), factory_);
    // We keep our own ref so refresh_launch can re-arm after the mount
    // points object unrefs.
    factory_ = static_cast<GstRTSPMediaFactory*>(g_object_ref(factory_));
}

void MountController::refresh_launch(const std::string& launch) {
    if (factory_ == nullptr)
        return;
    gst_rtsp_media_factory_set_launch(factory_, launch.c_str());
    XLOGF(INFO, "%s: launch refreshed: %s", mount_path_.c_str(),
              launch.c_str());
}

bool MountController::set_source_property(const char* property,
                                          const char* value) {
    auto* src = static_cast<GstElement*>(g_weak_ref_get(&source_));
    if (src == nullptr)
        return false;
    gst_util_set_object_arg(G_OBJECT(src), property, value);
    gst_object_unref(src);
    return true;
}

StreamStatus MountController::status() const {
    StreamStatus s;
    s.mounted = mounted();
    s.frames = frames_.load(std::memory_order_relaxed);
    s.fps = fps_.load(std::memory_order_relaxed);
    s.sequence = last_sequence_.load(std::memory_order_relaxed);
    s.pts = last_pts_.load(std::memory_order_relaxed);
    s.wallclock = last_wallclock_.load(std::memory_order_relaxed);
    if (auto* media = g_weak_ref_get(&const_cast<GWeakRef&>(media_))) {
        s.streaming = true;
        g_object_unref(media);
    }
    return s;
}

void MountController::disable() {
    dead_ = true;
    // Drop the weak refs so status() reports not-streaming and
    // set_source_property() refuses; the wedged pipeline itself is left
    // alone (see header).
    g_weak_ref_set(&media_, nullptr);
    g_weak_ref_set(&source_, nullptr);
    fps_.store(0, std::memory_order_relaxed);
}

bool MountController::check_stall(double configured_fps) {
    if (!mounted())
        return false;

    const guint64 frames = frames_.load(std::memory_order_relaxed);

    auto* media = static_cast<GstRTSPMedia*>(g_weak_ref_get(&media_));
    if (media == nullptr) {
        stalled_checks_ = 0;
        last_frames_ = frames;
        fps_.store(0, std::memory_order_relaxed);
        return false;
    }
    // Only a PLAYING pipeline is expected to produce frames -- prepared
    // (DESCRIBE'd) or paused media is not a stall.
    GstElement* bin = gst_rtsp_media_get_element(media);
    GstState state = GST_STATE_NULL;
    gst_element_get_state(bin, &state, nullptr, 0);
    gst_object_unref(bin);
    g_object_unref(media);

    if (state != GST_STATE_PLAYING || frames != last_frames_) {
        fps_.store(
            static_cast<double>(frames - last_frames_) / kWatchdogPeriodSec,
            std::memory_order_relaxed);
        // A live pipeline delivering far under its configured rate means
        // the sensor is dropping frames (usually Argus AE trading frame
        // rate for exposure in dim light -- see the ISP file's
        // autoFramerateRange).
        const double fps = fps_.load(std::memory_order_relaxed);
        if (state == GST_STATE_PLAYING && configured_fps > 0 &&
            fps < configured_fps * 0.8)
            XLOGF(WARN, "%s: delivering %.1f fps, configured %.0f",
                      mount_path_.c_str(), fps, configured_fps);
        stalled_checks_ = 0;
        last_frames_ = frames;
        return false;
    }

    fps_.store(0, std::memory_order_relaxed);
    if (++stalled_checks_ >= kStallChecks) {
        XLOGF(ERR, "watchdog: %s is live but produced no frame for %d s",
                   mount_path_.c_str(), kStallChecks * kWatchdogPeriodSec);
        return true;
    }
    XLOGF(WARN, "watchdog: %s produced no frame in the last %d s (%d/%d)",
              mount_path_.c_str(), kWatchdogPeriodSec, stalled_checks_,
              kStallChecks);
    return false;
}

}  // namespace camera
