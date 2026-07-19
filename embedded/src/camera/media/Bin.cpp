#include "camera/media/Bin.h"

#include "camera/base/logging/xlog.h"

namespace camera::media {

bool Bin::add(const std::shared_ptr<Element>& element) {
    if (element == nullptr || !element->valid()) return false;
    if (gst_bin_add(gst_bin(), element->gst()) == FALSE) {
        XLOGF(WARN, "media: %s could not take %s", name_.c_str(),
              element->name().c_str());
        return false;
    }
    elements_.push_back(element);
    return true;
}

bool Bin::add_linked(const std::vector<std::shared_ptr<Element>>& elements) {
    for (const auto& element : elements)
        if (!add(element)) return false;
    for (size_t i = 1; i < elements.size(); ++i)
        if (!elements[i - 1]->link(*elements[i])) return false;
    return true;
}

bool Bin::add_ghost_pad(const std::string& name, const Pad& target) {
    if (!target) return false;
    GstPad* ghost = gst_ghost_pad_new(name.c_str(), target.gst());
    if (ghost == nullptr) return false;
    if (gst_element_add_pad(gst(), ghost) == FALSE) {
        gst_object_unref(ghost);
        return false;
    }
    return true;
}

}  // namespace camera::media
