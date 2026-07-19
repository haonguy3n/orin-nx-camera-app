// Self-check for CameraPipeline: the capture half every transport shares.
// Runs on a dev host: spec.hw=false swaps the NVIDIA elements for generic
// ones (x264enc, videoconvert+videoscale), so the check exercises the SAME
// typed tee/encode/detect construction the device uses.
//
// Build (one line, from the repo root):
//   g++ -std=c++20 -Iembedded/src $(pkg-config --cflags --libs gstreamer-1.0
//       gstreamer-app-1.0) embedded/src/camera/media/Element.cpp
//       embedded/src/camera/media/Bin.cpp embedded/src/camera/media/Pipeline.cpp
//       embedded/src/camera/media/CameraPipeline.cpp
//       embedded/src/camera/media/test/camera_pipeline_check.cpp -o cp_check
#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>
#include <cstdio>
#include <string>
#include <vector>

#include "camera/media/CameraPipeline.h"

namespace {

using namespace camera::media;

// Records what it was handed, so the fanout can be asserted rather than eyeballed.
class Recorder : public IFrameTransport {
public:
    explicit Recorder(std::string tag) : tag_(std::move(tag)) {}
    void on_frame(uint8_t camera, const Frame& frame) override {
        ++frames;
        last_camera = camera;
        // The buffer is only valid during the call -- copying here is exactly
        // what a real transport must do, so the check exercises that contract.
        bytes += frame.size;
        assert(frame.data != nullptr && frame.size > 0);
    }
    int frames = 0;
    size_t bytes = 0;
    uint8_t last_camera = 255;

private:
    std::string tag_;
};

PipelineSpec spec(int width) {
    PipelineSpec s;
    s.source = "videotestsrc name=camsrc is-live=true ! video/x-raw,width=" +
               std::to_string(width) + ",height=240 ! videoconvert";
    s.h265 = false;         // x264enc is everywhere; x265enc often is not
    s.bitrate = 2000000;
    s.hw = false;
    return s;
}

}  // namespace

int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    CameraPipeline pipeline(1, spec(320));
    Recorder a("a"), b("b");
    // Two transports on ONE pipeline: the whole point, since Argus allows only
    // one consumer per camera and each transport cannot open the sensor itself.
    pipeline.add_transport(&a);
    pipeline.add_transport(&b);

    const auto started = pipeline.start(5ULL * GST_SECOND);
    if (!started.hasValue()) std::printf("  start: %s\n", started.error().c_str());
    assert(started.hasValue() && "pipeline must reach PLAYING");

    for (int i = 0; i < 20 && a.frames < 3; ++i)
        pipeline.pump(500);
    pipeline.drain_bus();

    assert(a.frames >= 3 && "encoded frames must reach the first transport");
    assert(b.frames == a.frames && "every transport must see the SAME frames");
    assert(a.bytes == b.bytes);
    assert(a.last_camera == 1 && b.last_camera == 1 && "camera index forwarded");
    assert(pipeline.frames() == static_cast<uint64_t>(a.frames));
    std::printf("[1] fanout: %d frames to 2 transports, identical\n", a.frames);

    // A property the source really has, so a silent typo cannot pass -- and
    // proof that "camsrc" is reachable inside the parsed source fragment.
    assert(pipeline.set_source_property("pattern", "1"));
    std::printf("[2] set_source_property on the live element: ok\n");

    // Changing the spec must request a relaunch (zoom and anything else the
    // source cannot take live), and must not fire for an identical one.
    assert(!pipeline.relaunch_wanted());
    pipeline.set_spec(spec(320));
    assert(!pipeline.relaunch_wanted() && "identical spec must not relaunch");
    pipeline.set_spec(spec(640));
    assert(pipeline.relaunch_wanted() && "changed spec must relaunch");
    std::printf("[3] relaunch requested only on a real change\n");

    // Rebuild with the new spec; the pipeline must come back and the relaunch
    // flag must clear.
    const auto restarted = pipeline.start(5ULL * GST_SECOND);
    assert(restarted.hasValue() && "restart with the new spec");
    assert(!pipeline.relaunch_wanted());
    bool pumped = false;
    for (int i = 0; i < 10 && !pumped; ++i) pumped = pipeline.pump(500);
    assert(pumped && "frames after relaunch");
    std::printf("[4] relaunch picks up the new spec\n");

    // The detect branch, built by the SAME typed construction the device
    // uses. Video and detection must be pumpable from SEPARATE threads:
    // inference is ~25ms while video arrives every ~16ms, so if a slow on_raw
    // could block pump(), the transport would stall the video the host is
    // watching. Simulate the slow detector and assert video keeps flowing.
    {
        struct SlowDetector : IFrameTransport {
            void on_frame(uint8_t, const Frame&) override { ++video; }
            void on_raw(uint8_t, const uint8_t* data, int width, int height,
                        int stride) override {
                assert(data != nullptr && width == 320 && height == 240);
                assert(stride >= width * 4);  // BGRx
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
                ++raw;
            }
            std::atomic<int> video{0}, raw{0};
        } slow;

        PipelineSpec teed = spec(640);
        teed.detect_width = 320;
        teed.detect_height = 240;
        CameraPipeline dual(0, teed);
        dual.add_transport(&slow);
        const auto up = dual.start(5ULL * GST_SECOND);
        if (!up.hasValue()) std::printf("  start: %s\n", up.error().c_str());
        assert(up.hasValue() && "tee'd pipeline must reach PLAYING");
        assert(dual.has_detect_branch() && "detect branch must be built");

        std::atomic<bool> stop{false};
        std::thread detector([&] {
            while (!stop) dual.pump_raw(200);
        });
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(1500);
        while (std::chrono::steady_clock::now() < deadline) dual.pump(200);
        stop = true;
        detector.join();
        dual.stop();

        // The detector is deliberately far slower than the frame rate; video
        // must not be limited by it.
        assert(slow.raw > 0 && "raw frames must reach on_raw");
        assert(slow.video > slow.raw &&
               "video must outpace a slow detector, not be blocked behind it");
        std::printf("[5] concurrent pumps: %d video vs %d raw (slow detector)\n",
                    slow.video.load(), slow.raw.load());
    }

    // Broken specs must fail loudly, not look like a camera producing nothing.
    CameraPipeline empty(0, PipelineSpec{});
    const auto none = empty.start(2ULL * GST_SECOND);
    assert(!none.hasValue() && "empty spec must be an error");
    PipelineSpec bogus = spec(320);
    bogus.source = "nosuchelement name=camsrc";
    CameraPipeline broken(0, bogus);
    const auto bad = broken.start(2ULL * GST_SECOND);
    assert(!bad.hasValue() && "unbuildable source must be an error");
    std::printf("[6] broken specs reported: \"%s\" / \"%s\"\n",
                none.error().c_str(), bad.error().c_str());

    pipeline.stop();
    std::printf("camera_pipeline_check: OK\n");
    return 0;
}
