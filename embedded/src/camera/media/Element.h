// A GstElement handle with fluent property/link helpers.
//
// Ported from the edge-streaming reference's media/models/Element, trimmed to
// what this device builds (no codec/profile negotiation, no signal plumbing
// beyond what the pipeline needs) and adapted to this codebase: no exceptions
// (nothing in camera:: throws), so construction failure surfaces as a null
// element that ElementFactory reports via base::Expected.
//
// Ownership: an Element does NOT own its GstElement. gst_element_factory_make
// returns a floating ref that gst_bin_add sinks, so the Pipeline owns every
// element added to it. Elements must be added to a bin or they leak.
#pragma once

#include <gst/gst.h>

#include <string>

#include "camera/media/Pad.h"

namespace camera::media {

class Element {
public:
    explicit Element(GstElement* element) : element_(element) {}

    // Property setters. Distinct overloads rather than a template because
    // g_object_set is varargs and needs the exact C type -- and because a
    // bare const char* would otherwise bind to the bool overload (pointer ->
    // bool is a standard conversion, but -> std::string is user-defined), so
    // set("format", "BGR") would silently set a boolean TRUE.
    Element* set(const std::string& prop, const char* value);
    Element* set(const std::string& prop, const std::string& value);
    Element* set(const std::string& prop, bool value);
    Element* set(const std::string& prop, int value);
    Element* set(const std::string& prop, guint value);
    Element* set(const std::string& prop, gint64 value);
    Element* set(const std::string& prop, double value);

    // capsfilter "caps" from a caps string. No-op on an unparseable string,
    // which is reported rather than silently leaving caps unset.
    Element* set_caps(const std::string& caps);

    [[nodiscard]] Pad static_pad(const std::string& name) const;
    [[nodiscard]] Pad request_pad(const std::string& name) const;

    // Links this element's src to `sink`'s sink. False when GStreamer refuses
    // (incompatible caps) -- the caller decides whether that is fatal.
    [[nodiscard]] bool link(const Element& sink) const;

    [[nodiscard]] GstElement* gst() const { return element_; }
    [[nodiscard]] std::string name() const;
    [[nodiscard]] bool valid() const { return element_ != nullptr; }

    bool sync_state_with_parent() const;

private:
    GstElement* element_ = nullptr;
};

}  // namespace camera::media
