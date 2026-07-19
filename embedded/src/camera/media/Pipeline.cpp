#include "camera/media/Pipeline.h"

namespace camera::media {

Pipeline::~Pipeline() {
    if (gst() == nullptr) return;
    gst_element_set_state(gst(), GST_STATE_NULL);
    gst_object_unref(gst());
}

PipelineResult Pipeline::play(guint64 timeout_ns) {
    if (gst() == nullptr)
        return base::makeUnexpected(std::string{"pipeline was never built"});

    if (gst_element_set_state(gst(), GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE)
        return base::makeUnexpected(std::string{"pipeline refused to start"});

    // Judge by the state reached, not the return code: a live source can
    // report NO_PREROLL while genuinely running, and treating that as failure
    // would break a working pipeline.
    GstState state = GST_STATE_NULL;
    gst_element_get_state(gst(), &state, nullptr, timeout_ns);
    if (state != GST_STATE_PLAYING)
        return base::makeUnexpected(
            std::string{"pipeline did not reach PLAYING (stuck in "} +
            gst_element_state_get_name(state) + "); a branch is not prerolling");

    return base::Unit{};
}

void Pipeline::stop() {
    if (gst() != nullptr) gst_element_set_state(gst(), GST_STATE_NULL);
}

}  // namespace camera::media
