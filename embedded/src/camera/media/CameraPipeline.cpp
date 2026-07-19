#include "camera/media/CameraPipeline.h"

#include "camera/base/logging/xlog.h"
#include "camera/media/Bin.h"
#include "camera/media/ElementFactory.h"
#include "camera/media/Tee.h"

namespace camera::media {

CameraPipeline::CameraPipeline(uint8_t camera, PipelineSpec spec)
    : camera_(camera), spec_(std::move(spec)) {}

CameraPipeline::~CameraPipeline() { stop(); }

void CameraPipeline::add_transport(IFrameTransport* transport) {
    if (transport == nullptr) return;
    std::lock_guard<std::mutex> lock(mutex_);
    transports_.push_back(transport);
}

PipelineSpec CameraPipeline::spec() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return spec_;
}

void CameraPipeline::set_spec(PipelineSpec spec) {
    if (spec.source.empty()) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (spec == spec_) return;
        spec_ = std::move(spec);
    }
    relaunch_ = true;
}

// Assembles source-fragment -> [tee ->] encode -> "sink" appsink, plus the
// detect branch when asked for. Every element is created and linked as an
// object: a missing element or refused link is reported by name instead of
// gst_parse_launch's silently-partial pipeline (which is how a missing detect
// branch once hid while video kept working).
PipelineResult CameraPipeline::build(const PipelineSpec& spec) {
    if (spec.source.empty())
        return base::makeUnexpected(std::string("no source description"));

    pipeline_ = std::make_unique<Pipeline>(
        "cam" + std::to_string(static_cast<unsigned>(camera_)));

    // The source stays a parsed fragment (config-driven, logged for
    // gst-launch repro). ghost-pads=TRUE exposes its unlinked src pad so the
    // bin links like a plain element.
    GError* failure = nullptr;
    GstElement* parsed = gst_parse_bin_from_description(spec.source.c_str(),
                                                        TRUE, &failure);
    if (parsed == nullptr) {
        std::string message =
            failure != nullptr ? failure->message : "unknown parse error";
        if (failure != nullptr) g_error_free(failure);
        return base::makeUnexpected("source: " + std::move(message));
    }
    // A returned bin can still carry a non-fatal error: an element that could
    // not be created, with that part of the graph quietly omitted. That is
    // fatal here -- the fragment is exactly what the config asked for.
    if (failure != nullptr) {
        std::string message = failure->message;
        g_error_free(failure);
        gst_object_unref(parsed);
        return base::makeUnexpected("source built partially: " + message);
    }
    auto source = std::make_shared<Element>(parsed);
    if (!pipeline_->add(source))
        return base::makeUnexpected(std::string("could not add source bin"));

    // Encode chain: queue ! enc ! parse ! caps ! appsink "sink".
    auto queue = ElementFactory::create("queue", "venc_queue");
    auto enc = ElementFactory::create(
        spec.hw ? (spec.h265 ? "nvv4l2h265enc" : "nvv4l2h264enc")
                : (spec.h265 ? "x265enc" : "x264enc"),
        "venc");
    auto parse = ElementFactory::create(spec.h265 ? "h265parse" : "h264parse",
                                        "vparse");
    auto caps = ElementFactory::create("capsfilter", "vcaps");
    auto sink = ElementFactory::create("appsink", "sink");
    for (const auto* e : {&queue, &enc, &parse, &caps, &sink})
        if (!e->hasValue()) return base::makeUnexpected(e->error());

    if (spec.hw) {
        enc.value()
            ->set("bitrate", spec.bitrate)
            ->set("insert-sps-pps", true)
            ->set("idrinterval", 30)
            ->set("maxperf-enable", true);
    } else {
        // x26Xenc takes kbit/s.
        enc.value()
            ->set("bitrate", static_cast<guint>(spec.bitrate / 1000))
            ->set("key-int-max", 30)
            ->set_from_string("tune", "zerolatency");
    }
    parse.value()->set("config-interval", -1);
    caps.value()->set_caps(spec.h265 ? "video/x-h265,stream-format=byte-stream"
                                     : "video/x-h264,stream-format=byte-stream");
    sink.value()
        ->set("sync", false)
        ->set("max-buffers", static_cast<guint>(8))
        ->set("drop", true);

    if (!pipeline_->add_linked({queue.value(), enc.value(), parse.value(),
                                caps.value(), sink.value()}))
        return base::makeUnexpected(std::string("encode chain did not link"));

    if (spec.detect_width > 0 && spec.detect_height > 0) {
        // Detect branch off a tee. The leaky single-buffer queue is
        // load-bearing: a slow detector must drop frames on ITS branch, never
        // backpressure through the tee into the video the host is watching.
        // async=false on the appsink pairs with it -- the leaky queue drops
        // the preroll buffer, so without it the sink never prerolls and the
        // WHOLE pipeline sticks in PAUSED. drop=false keeps the sensor cool:
        // blocking means the converter only works when the detector pulls
        // (measured: VIC ~70% + GR3D ~40% continuous with drop=true).
        auto tee = ElementFactory::create("tee", "vtee");
        auto dqueue = ElementFactory::create("queue", "detect_queue");
        auto dcaps = ElementFactory::create("capsfilter", "detect_caps");
        auto dsink = ElementFactory::create("appsink", "detect");
        for (const auto* e : {&tee, &dqueue, &dcaps, &dsink})
            if (!e->hasValue()) return base::makeUnexpected(e->error());

        dqueue.value()
            ->set_from_string("leaky", "downstream")
            ->set("max-size-buffers", static_cast<guint>(1));
        dcaps.value()->set_caps(
            "video/x-raw,format=BGRx,width=" +
            std::to_string(spec.detect_width) +
            ",height=" + std::to_string(spec.detect_height));
        dsink.value()
            ->set("sync", false)
            ->set("async", false)
            ->set("max-buffers", static_cast<guint>(1))
            ->set("drop", false);

        // nvvidconv does NVMM->CPU download and the rescale in one element;
        // generic hosts need videoconvert + videoscale for the same edge.
        std::vector<std::shared_ptr<Element>> branch{dqueue.value()};
        if (spec.hw) {
            auto conv = ElementFactory::create("nvvidconv", "detect_conv");
            if (!conv.hasValue()) return base::makeUnexpected(conv.error());
            branch.push_back(conv.value());
        } else {
            auto conv = ElementFactory::create("videoconvert", "detect_conv");
            auto scale = ElementFactory::create("videoscale", "detect_scale");
            for (const auto* e : {&conv, &scale})
                if (!e->hasValue()) return base::makeUnexpected(e->error());
            branch.push_back(conv.value());
            branch.push_back(scale.value());
        }
        branch.push_back(dcaps.value());
        branch.push_back(dsink.value());

        if (!pipeline_->add(tee.value()) || !source->link(*tee.value()))
            return base::makeUnexpected(std::string("source -> tee did not link"));
        if (!pipeline_->add_linked(branch))
            return base::makeUnexpected(std::string("detect branch did not link"));

        const Tee fanout(tee.value());
        if (!fanout.link_branch(queue.value()->static_pad("sink")) ||
            !fanout.link_branch(dqueue.value()->static_pad("sink")))
            return base::makeUnexpected(std::string("tee branch did not link"));

        detect_ = GST_APP_SINK(dsink.value()->gst());
    } else if (!source->link(*queue.value())) {
        return base::makeUnexpected(std::string("source -> encoder did not link"));
    }

    sink_ = GST_APP_SINK(sink.value()->gst());
    // Inside the parsed fragment; named by every source builder so runtime
    // properties (AE ranges) reach the live element.
    source_ = gst_bin_get_by_name(GST_BIN(parsed), "camsrc");
    return base::Unit{};
}

PipelineResult CameraPipeline::start(guint64 timeout_ns) {
    stop();
    relaunch_ = false;

    if (auto built = build(spec()); !built.hasValue()) {
        stop();
        return built;
    }
    if (auto playing = pipeline_->play(timeout_ns); !playing.hasValue()) {
        stop();
        return playing;
    }
    return base::Unit{};
}

void CameraPipeline::stop() {
    if (source_ != nullptr) {
        gst_object_unref(source_);
        source_ = nullptr;
    }
    // sink_/detect_ are owned by the pipeline bin; dropping it drops them.
    sink_ = nullptr;
    detect_ = nullptr;
    pipeline_.reset();
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

bool CameraPipeline::pump_raw(int timeout_ms) {
    if (detect_ == nullptr) return false;
    GstSample* raw = gst_app_sink_try_pull_sample(
        detect_, static_cast<GstClockTime>(timeout_ms) * GST_MSECOND);
    if (raw == nullptr) return false;
    deliver_raw(raw);
    gst_sample_unref(raw);
    return true;
}

bool CameraPipeline::pump(int timeout_ms) {
    if (sink_ == nullptr) return false;
    const GstClockTime timeout =
        static_cast<GstClockTime>(timeout_ms) * GST_MSECOND;
    GstSample* sample = gst_app_sink_try_pull_sample(sink_, timeout);
    if (sample == nullptr) return false;
    deliver(sample);
    gst_sample_unref(sample);
    return true;
}

bool CameraPipeline::is_eos() const {
    return sink_ != nullptr && gst_app_sink_is_eos(sink_) == TRUE;
}

bool CameraPipeline::set_source_property(const char* property,
                                         const char* value) {
    if (source_ == nullptr) return false;
    gst_util_set_object_arg(G_OBJECT(source_), property, value);
    return true;
}

void CameraPipeline::drain_bus() {
    if (pipeline_ == nullptr) return;
    GstBus* bus = pipeline_->bus();
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
