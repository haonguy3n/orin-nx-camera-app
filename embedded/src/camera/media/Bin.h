// A GstBin: a container of Elements.
//
// Ported from the edge-streaming reference's media/models/Bin, trimmed: the
// reference's codec Connect() overloads and teardown/target-bin machinery
// serve its dynamic multi-output (WebRTC/HLS/snapshot) case, which a fixed
// dual-camera device has no use for.
//
// The bin keeps the Element wrappers alive; the underlying GstElements are
// owned by the GstBin itself once added.
#pragma once

#include <gst/gst.h>

#include <memory>
#include <string>
#include <vector>

#include "camera/media/Element.h"

namespace camera::media {

class Bin : public Element {
public:
    explicit Bin(const std::string& name)
        : Bin(name, gst_bin_new(name.c_str())) {}

    [[nodiscard]] GstBin* gst_bin() const { return GST_BIN(gst()); }

    // Adds `element` to the bin and keeps its wrapper alive. Returns false if
    // GStreamer rejects it (already in another bin).
    bool add(const std::shared_ptr<Element>& element);

    // Adds all of `elements` and links them in order. Returns false on the
    // first add or link that fails, so a half-built chain is reported rather
    // than left to fail later as "no frames".
    bool add_linked(const std::vector<std::shared_ptr<Element>>& elements);

    // Exposes `pad` on the bin's boundary under `name`, so a bin built from a
    // parsed description can be linked like a plain element.
    bool add_ghost_pad(const std::string& name, const Pad& target);

protected:
    // For subclasses wrapping an existing GstElement (Pipeline wraps a
    // GstPipeline, which is itself a GstBin).
    Bin(std::string name, GstElement* element)
        : Element(element), name_(std::move(name)) {}

    std::string name_;

private:
    std::vector<std::shared_ptr<Element>> elements_;
};

}  // namespace camera::media
