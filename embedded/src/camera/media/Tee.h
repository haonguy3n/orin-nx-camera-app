// A tee and its output branches.
//
// Ported from the edge-streaming reference's media/models/Tee, trimmed hard:
// the reference splits audio+video by codec and profile for dynamic
// subscribers, none of which a fixed camera needs. What survives is the part
// that was worth having -- request pads handed out as typed objects instead of
// "tee name=t  t. ! ..." string concatenation, which is where the tee bugs on
// this device actually came from.
#pragma once

#include <memory>
#include <string>

#include "camera/media/Element.h"
#include "camera/media/Pad.h"

namespace camera::media {

class Tee {
public:
    explicit Tee(std::shared_ptr<Element> element)
        : element_(std::move(element)) {}

    [[nodiscard]] const std::shared_ptr<Element>& element() const {
        return element_;
    }

    // Requests a new output pad. Each branch gets its own; the tee fans the
    // stream to all of them.
    [[nodiscard]] Pad branch() const { return element_->request_pad("src_%u"); }

    // Requests a branch and links it to `sink`. False if the tee has no pad to
    // give or the link is refused.
    [[nodiscard]] bool link_branch(const Pad& sink) const {
        const Pad src = branch();
        return src && src.link(sink);
    }

private:
    std::shared_ptr<Element> element_;
};

}  // namespace camera::media
