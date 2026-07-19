#include "camera/media/Element.h"

#include "camera/base/logging/xlog.h"

namespace camera::media {

Element* Element::set(const std::string& prop, const char* value) {
    if (element_ != nullptr)
        g_object_set(element_, prop.c_str(), value, nullptr);
    return this;
}

Element* Element::set(const std::string& prop, const std::string& value) {
    return set(prop, value.c_str());
}

Element* Element::set(const std::string& prop, bool value) {
    if (element_ != nullptr)
        g_object_set(element_, prop.c_str(), static_cast<gboolean>(value),
                     nullptr);
    return this;
}

Element* Element::set(const std::string& prop, int value) {
    if (element_ != nullptr)
        g_object_set(element_, prop.c_str(), value, nullptr);
    return this;
}

Element* Element::set(const std::string& prop, guint value) {
    if (element_ != nullptr)
        g_object_set(element_, prop.c_str(), value, nullptr);
    return this;
}

Element* Element::set(const std::string& prop, gint64 value) {
    if (element_ != nullptr)
        g_object_set(element_, prop.c_str(), value, nullptr);
    return this;
}

Element* Element::set(const std::string& prop, double value) {
    if (element_ != nullptr)
        g_object_set(element_, prop.c_str(), value, nullptr);
    return this;
}

Element* Element::set_from_string(const std::string& prop,
                                  const std::string& value) {
    if (element_ != nullptr)
        gst_util_set_object_arg(G_OBJECT(element_), prop.c_str(),
                                value.c_str());
    return this;
}

Element* Element::set_caps(const std::string& caps) {
    if (element_ == nullptr) return this;
    GstCaps* parsed = gst_caps_from_string(caps.c_str());
    if (parsed == nullptr) {
        XLOGF(WARN, "media: unparseable caps on %s: %s", name().c_str(),
              caps.c_str());
        return this;
    }
    g_object_set(element_, "caps", parsed, nullptr);
    gst_caps_unref(parsed);
    return this;
}

Pad Element::static_pad(const std::string& name) const {
    if (element_ == nullptr) return Pad{};
    return Pad{gst_element_get_static_pad(element_, name.c_str())};
}

Pad Element::request_pad(const std::string& name) const {
    if (element_ == nullptr) return Pad{};
    return Pad{gst_element_request_pad_simple(element_, name.c_str())};
}

bool Element::link(const Element& sink) const {
    if (element_ == nullptr || sink.element_ == nullptr) return false;
    if (gst_element_link(element_, sink.element_) == FALSE) {
        XLOGF(WARN, "media: failed to link %s -> %s", name().c_str(),
              sink.name().c_str());
        return false;
    }
    return true;
}

std::string Element::name() const {
    if (element_ == nullptr) return {};
    gchar* raw = gst_element_get_name(element_);
    std::string result = raw != nullptr ? raw : "";
    g_free(raw);
    return result;
}

bool Element::sync_state_with_parent() const {
    return element_ != nullptr &&
           gst_element_sync_state_with_parent(element_) == TRUE;
}

}  // namespace camera::media
