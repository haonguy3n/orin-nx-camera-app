// A GstPipeline: the top-level Bin, with state control and the bus.
//
// Ported from the edge-streaming reference's media/models/Pipeline. Two
// departures, both paid for in device debugging:
//
//  - play() waits for the state to settle instead of returning the state
//    change code. set_state returns ASYNC for a live source, so "no failure"
//    never meant "running": a branch that fails to preroll leaves the whole
//    pipeline in PAUSED, which looks exactly like a camera producing nothing
//    while also hanging teardown.
//  - It reports base::Expected rather than throwing, matching camera::.
#pragma once

#include <gst/gst.h>

#include <string>

#include "camera/base/Expected.h"
#include "camera/base/Unit.h"
#include "camera/media/Bin.h"

namespace camera::media {

using PipelineResult = base::Expected<base::Unit, std::string>;

class Pipeline : public Bin {
public:
    explicit Pipeline(const std::string& name)
        : Bin(name, gst_pipeline_new(name.c_str())) {}

    ~Pipeline();

    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    // Goes to PLAYING and waits up to `timeout_ns` for the state to settle.
    // Fails with the state actually reached, so a non-prerolling branch names
    // itself instead of stalling silently. Default covers Argus startup.
    PipelineResult play(guint64 timeout_ns = 10ULL * GST_SECOND);

    void stop();

    // Caller unrefs. Errors are drained by the caller, which knows which
    // camera to attribute them to.
    [[nodiscard]] GstBus* bus() const { return gst_element_get_bus(gst()); }
};

}  // namespace camera::media
