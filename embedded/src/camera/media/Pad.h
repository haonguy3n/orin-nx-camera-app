// A GstPad handle.
//
// Ported from the edge-streaming reference's media/models/Pad, with one
// change: this owns its reference. gst_element_get_static_pad and
// request_pad_simple both return a NEW ref, and the reference implementation
// never dropped it. Camera sessions rebuild their pipeline on every reconnect,
// so leaked pad refs would accumulate for the life of the service.
#pragma once

#include <gst/gst.h>

#include <string>
#include <utility>

namespace camera::media {

class Pad {
public:
    Pad() = default;
    // Takes ownership of `pad` (the new ref returned by the gst getters).
    explicit Pad(GstPad* pad) : pad_(pad) {}

    ~Pad() { reset(); }

    Pad(const Pad& other)
        : pad_(other.pad_ != nullptr ? GST_PAD(gst_object_ref(other.pad_))
                                     : nullptr) {}
    Pad(Pad&& other) noexcept : pad_(std::exchange(other.pad_, nullptr)) {}

    Pad& operator=(Pad other) noexcept {
        std::swap(pad_, other.pad_);
        return *this;
    }

    [[nodiscard]] GstPad* gst() const { return pad_; }
    explicit operator bool() const { return pad_ != nullptr; }

    // Links this (src) pad to `sink`. False when either side is absent or
    // GStreamer refuses the link (incompatible caps).
    [[nodiscard]] bool link(const Pad& sink) const {
        if (pad_ == nullptr || sink.pad_ == nullptr) return false;
        return gst_pad_link(pad_, sink.pad_) == GST_PAD_LINK_OK;
    }

    [[nodiscard]] std::string name() const {
        if (pad_ == nullptr) return {};
        gchar* raw = gst_pad_get_name(pad_);
        std::string result = raw != nullptr ? raw : "";
        g_free(raw);
        return result;
    }

private:
    void reset() {
        if (pad_ != nullptr) gst_object_unref(pad_);
        pad_ = nullptr;
    }

    GstPad* pad_ = nullptr;
};

}  // namespace camera::media
