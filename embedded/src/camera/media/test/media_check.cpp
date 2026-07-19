// Self-check for camera::media, runnable on a dev host (no Argus needed).
//
// It builds the same shape the secure-USB transport uses -- source, tee, an
// encode-style branch and a best-effort detect-style branch behind a leaky
// queue -- with generic elements standing in for the NVIDIA ones.
//
// The invariant it guards is the bug that cost this device three flash cycles:
// a sink gates the pipeline's async state change until it prerolls, but the
// detect branch's queue is leaky=downstream max-size-buffers=1 and can drop
// exactly that preroll buffer. The sink then never prerolls, the WHOLE
// pipeline stays in PAUSED, and no frames reach the encode branch either.
// async=false takes the best-effort sink out of the state change.
//
// Build (host):
//   g++ -std=c++20 -Iembedded/src $(pkg-config --cflags --libs gstreamer-1.0) \
//       embedded/src/camera/media/*.cpp \
//       embedded/src/camera/media/test/media_check.cpp -o media_check
#include <cassert>
#include <cstdio>
#include <memory>
#include <string>

#include "camera/media/Bin.h"
#include "camera/media/ElementFactory.h"
#include "camera/media/Pipeline.h"
#include "camera/media/Tee.h"

namespace {

using namespace camera::media;

std::shared_ptr<Element> make(const std::string& factory) {
    auto result = ElementFactory::create(factory);
    assert(result.hasValue() && "element factory failed");
    return result.value();
}

// Builds source -> tee -> (queue,fakesink) + (leaky queue, best-effort sink).
// `best_effort_async` is the property under test on the second branch's sink.
bool reaches_playing(bool best_effort_async) {
    Pipeline pipeline("check");

    auto source = make("videotestsrc");
    source->set("is-live", true);
    auto tee_element = make("tee");
    assert(pipeline.add(source) && pipeline.add(tee_element));
    assert(source->link(*tee_element));
    const Tee tee(tee_element);

    // Branch 1: stands in for the encode/appsink leg. Ordinary blocking queue.
    auto queue = make("queue");
    auto sink = make("fakesink");
    sink->set("sync", false);
    assert(pipeline.add_linked({queue, sink}));
    assert(tee.link_branch(queue->static_pad("sink")));

    // Branch 2: stands in for the detect leg -- leaky, so it may drop the
    // buffer its sink would otherwise preroll on.
    auto detect_queue = make("queue");
    detect_queue->set("leaky", 2 /* downstream */);
    detect_queue->set("max-size-buffers", 1u);
    detect_queue->set("max-size-bytes", 0u);
    detect_queue->set("max-size-time", static_cast<gint64>(0));
    auto detect_sink = make("fakesink");
    detect_sink->set("sync", false);
    detect_sink->set("async", best_effort_async);
    assert(pipeline.add_linked({detect_queue, detect_sink}));
    assert(tee.link_branch(detect_queue->static_pad("sink")));

    const auto played = pipeline.play(5ULL * GST_SECOND);
    if (!played.hasValue()) std::printf("    play: %s\n", played.error().c_str());
    pipeline.stop();
    return played.hasValue();
}

}  // namespace

int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    // The invariant: a best-effort branch taken out of the async state change
    // cannot hold the pipeline out of PLAYING, however hard its queue leaks.
    std::printf("[1] best-effort sink with async=false\n");
    const bool with_async_false = reaches_playing(false);
    assert(with_async_false && "async=false branch must not gate PLAYING");
    std::printf("    reached PLAYING\n");

    // Informational, deliberately not asserted: whether the default (async=true)
    // sink actually stalls here depends on whether the queue wins the race to
    // drop the preroll buffer, which is timing-dependent on a fast host. On the
    // device it lost that race every time. Printed so a regression in the
    // gating behaviour is at least visible.
    std::printf("[2] same branch with default async=true (informational)\n");
    std::printf("    reached PLAYING: %s\n",
                reaches_playing(true) ? "yes" : "no -- stalled, as on device");

    std::printf("media_check: OK\n");
    return 0;
}
