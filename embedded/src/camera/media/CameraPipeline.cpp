#include "camera/media/CameraPipeline.h"

#include "camera/base/logging/xlog.h"

namespace camera::media {

CameraPipeline::CameraPipeline(uint8_t camera, std::string description)
    : camera_(camera), description_(std::move(description)) {}

CameraPipeline::~CameraPipeline() { stop(); }

void CameraPipeline::add_transport(IFrameTransport* transport) {
    if (transport == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    transports_.push_back(transport);
}

std::string CameraPipeline::description() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return description_;
}

void CameraPipeline::set_description(std::string description) {
    if (description.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (description == description_) return;
        description_ = std::move(description);
    }
    relaunch_ = true;
}

PipelineResult CameraPipeline::start(guint64 timeout_ns) {
    stop();
    relaunch_ = false;
    const std::string launch = description();

    GError* failure = nullptr;
    pipeline_ = gst_parse_launch(launch.c_str(), &failure);
    if (pipeline_ == nullptr) {
        std::string message =
            failure != nullptr ? failure->message : "unknown parse error";
        if (failure != nullptr) g_error_free(failure);
        return base::makeUnexpected(std::move(message));
    }
    // A returned pipeline can still carry a non-fatal error: an element that
    // could not be created, with that part of the graph quietly omitted.
    // Discarding it is how a missing detect branch stayed invisible while
    // video kept working.
    if (failure != nullptr) {
        XLOGF(WARN, "cam%u pipeline built with warnings: %s",
              static_cast<unsigned>(camera_), failure->message);
        g_error_free(failure);
    }

    if (GstElement* s = gst_bin_get_by_name(GST_BIN(pipeline_), "sink"))
        sink_ = GST_APP_SINK(s);
    if (sink_ == nullptr) {
        stop();
        return base::makeUnexpected(std::string("pipeline has no 'sink' appsink"));
    }
    if (GstElement* d = gst_bin_get_by_name(GST_BIN(pipeline_), "detect"))
        detect_ = GST_APP_SINK(d);
    source_ = gst_bin_get_by_name(GST_BIN(pipeline_), "camsrc");

    if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        stop();
        return base::makeUnexpected(std::string("pipeline refused to start"));
    }
    // Judge by the state reached, not the return code: a live source can
    // report NO_PREROLL while genuinely running.
    GstState state = GST_STATE_NULL;
    gst_element_get_state(pipeline_, &state, nullptr, timeout_ns);
    if (state != GST_STATE_PLAYING) {
        std::string message =
            std::string("pipeline did not reach PLAYING (stuck in ") +
            gst_element_state_get_name(state) + "); a branch is not prerolling";
        stop();
        return base::makeUnexpected(std::move(message));
    }
    return base::Unit{};
}

void CameraPipeline::stop() {
    if (source_ != nullptr) {
        gst_object_unref(source_);
        source_ = nullptr;
    }
    if (sink_ != nullptr) {
        gst_object_unref(sink_);
        sink_ = nullptr;
    }
    if (detect_ != nullptr) {
        gst_object_unref(detect_);
        detect_ = nullptr;
    }
    if (pipeline_ != nullptr) {
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_object_unref(pipeline_);
        pipeline_ = nullptr;
    }
}

void CameraPipeline::deliver(GstSample* sample) {
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (buffer == nullptr) return;
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return;

    Frame frame;
    frame.data = map.data;
    frame.size = map.size;
    frame.pts = GST_BUFFER_PTS(buffer);
    frame.keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

    std::vector<IFrameTransport*> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targets = transports_;
    }
    for (IFrameTransport* transport : targets)
        transport->on_frame(camera_, frame);

    gst_buffer_unmap(buffer, &map);
    frames_.fetch_add(1, std::memory_order_relaxed);
}

void CameraPipeline::deliver_raw(GstSample* sample) {
    int width = 0, height = 0;
    if (GstCaps* caps = gst_sample_get_caps(sample)) {
        const GstStructure* s = gst_caps_get_structure(caps, 0);
        gst_structure_get_int(s, "width", &width);
        gst_structure_get_int(s, "height", &height);
    }
    GstBuffer* buffer = gst_sample_get_buffer(sample);
    if (width <= 0 || height <= 0 || buffer == nullptr) return;
    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return;
    const int stride = static_cast<int>(map.size) / height;

    std::vector<IFrameTransport*> targets;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        targets = transports_;
    }
    for (IFrameTransport* transport : targets)
        transport->on_raw(camera_, map.data, width, height, stride);

    gst_buffer_unmap(buffer, &map);
}

bool CameraPipeline::pump(int timeout_ms) {
    if (sink_ == nullptr) return false;
    const GstClockTime timeout =
        static_cast<GstClockTime>(timeout_ms) * GST_MSECOND;

    // The raw branch is best-effort and must never gate video: take whatever
    // is already there without waiting.
    if (detect_ != nullptr) {
        if (GstSample* raw = gst_app_sink_try_pull_sample(detect_, 0)) {
            deliver_raw(raw);
            gst_sample_unref(raw);
        }
    }

    GstSample* sample = gst_app_sink_try_pull_sample(sink_, timeout);
    if (sample == nullptr) return false;
    deliver(sample);
    gst_sample_unref(sample);
    return true;
}

bool CameraPipeline::set_source_property(const char* property,
                                         const char* value) {
    if (source_ == nullptr) return false;
    gst_util_set_object_arg(G_OBJECT(source_), property, value);
    return true;
}

void CameraPipeline::drain_bus() {
    if (pipeline_ == nullptr) return;
    GstBus* bus = gst_element_get_bus(pipeline_);
    if (bus == nullptr) return;
    while (GstMessage* message = gst_bus_pop_filtered(
               bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR |
                                                GST_MESSAGE_EOS))) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* failure = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &failure, &debug);
            XLOGF(WARN, "cam%u pipeline error: %s",
                  static_cast<unsigned>(camera_),
                  failure != nullptr ? failure->message : "unknown");
            if (failure != nullptr) g_error_free(failure);
            g_free(debug);
        }
        gst_message_unref(message);
    }
    gst_object_unref(bus);
}

}  // namespace camera::media
