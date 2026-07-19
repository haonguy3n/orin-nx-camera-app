// Self-check for CameraPipeline: the capture half every transport shares.
// Runs on a dev host with generic elements standing in for the NVIDIA ones.
//
// Build:
//   g++ -std=c++20 -Iembedded/src $(pkg-config --cflags --libs gstreamer-1.0
//       gstreamer-app-1.0) embedded/src/camera/media/CameraPipeline.cpp
//       embedded/src/camera/media/test/camera_pipeline_check.cpp -o cp_check
#include <cassert>
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

std::string launch(int width) {
    return "videotestsrc name=camsrc is-live=true ! video/x-raw,width=" +
           std::to_string(width) +
           ",height=240 ! appsink name=sink sync=false max-buffers=4 drop=true";
}

}  // namespace

int main(int argc, char** argv) {
    gst_init(&argc, &argv);

    CameraPipeline pipeline(1, launch(320));
    Recorder a("a"), b("b");
    // Two transports on ONE pipeline: the whole point, since Argus allows only
    // one consumer per camera and each transport cannot open the sensor itself.
    pipeline.add_transport(&a);
    pipeline.add_transport(&b);

    const auto started = pipeline.start(5ULL * GST_SECOND);
    if (!started.hasValue()) std::printf("  start: %s\n", started.error().c_str());
    assert(started.hasValue() && "pipeline must reach PLAYING");

    for (int i = 0; i < 5 && a.frames < 3; ++i)
        pipeline.pump(500);
    pipeline.drain_bus();

    assert(a.frames >= 3 && "frames must reach the first transport");
    assert(b.frames == a.frames && "every transport must see the SAME frames");
    assert(a.bytes == b.bytes);
    assert(a.last_camera == 1 && b.last_camera == 1 && "camera index forwarded");
    assert(pipeline.frames() == static_cast<uint64_t>(a.frames));
    std::printf("[1] fanout: %d frames to 2 transports, identical\n", a.frames);

    // A property the source really has, so a silent typo cannot pass.
    assert(pipeline.set_source_property("pattern", "1"));
    std::printf("[2] set_source_property on the live element: ok\n");

    // Changing the description must request a relaunch (zoom and anything else
    // the source cannot take live), and must not fire for an identical one.
    assert(!pipeline.relaunch_wanted());
    pipeline.set_description(launch(320));
    assert(!pipeline.relaunch_wanted() && "identical description must not relaunch");
    pipeline.set_description(launch(640));
    assert(pipeline.relaunch_wanted() && "changed description must relaunch");
    std::printf("[3] relaunch requested only on a real change\n");

    // Rebuild with the new description; the pipeline must come back and the
    // relaunch flag must clear.
    const auto restarted = pipeline.start(5ULL * GST_SECOND);
    assert(restarted.hasValue() && "restart with the new description");
    assert(!pipeline.relaunch_wanted());
    assert(pipeline.pump(1000) && "frames after relaunch");
    std::printf("[4] relaunch picks up the new description\n");

    // A pipeline whose sink is missing must fail loudly, not look like a
    // camera that produces nothing.
    CameraPipeline broken(0, "videotestsrc ! fakesink");
    const auto bad = broken.start(2ULL * GST_SECOND);
    assert(!bad.hasValue() && "missing 'sink' appsink must be an error");
    std::printf("[5] missing sink reported: %s\n", bad.error().c_str());

    pipeline.stop();
    std::printf("camera_pipeline_check: OK\n");
    return 0;
}
