// Named Element creation.
//
// Ported from the edge-streaming reference's media/factory/ElementFactory. The
// reference throws ElementCreationError; nothing in camera:: throws, so a
// missing plugin surfaces as base::Expected with the factory name in it --
// which is the message you want when a device image is missing an NVIDIA
// element.
#pragma once

#include <gst/gst.h>

#include <atomic>
#include <memory>
#include <string>

#include "camera/base/Expected.h"
#include "camera/media/Element.h"

namespace camera::media {

using ElementResult =
    base::Expected<std::shared_ptr<Element>, std::string>;

class ElementFactory {
public:
    // `desc` names the element in logs and bus errors; a counter keeps it
    // unique so two cameras' elements never collide inside one pipeline.
    static ElementResult create(const std::string& gst_factory,
                                const std::string& desc) {
        if (gst_factory.empty())
            return base::makeUnexpected(std::string{"empty factory name"});

        const std::string unique = desc + std::to_string(next_id());
        GstElement* raw =
            gst_element_factory_make(gst_factory.c_str(), unique.c_str());
        if (raw == nullptr)
            return base::makeUnexpected("no such GStreamer element: " +
                                        gst_factory);
        return std::make_shared<Element>(raw);
    }

    static ElementResult create(const std::string& gst_factory) {
        return create(gst_factory, gst_factory);
    }

private:
    static unsigned next_id() {
        static std::atomic<unsigned> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed);
    }
};

}  // namespace camera::media
