// Where detection results go.
//
// Detection itself is identical on every transport -- same detector, same
// frames, same JSON (detect::to_meta_json). Only delivery differs: the secure
// session enqueues on Channel::Meta, while a plain RTSP deployment has no
// session and must push over the control connection instead.
//
// Splitting that out is what lets network mode have face detection at all:
// before this, detection was welded to the secure USB Session type, so RTSP
// could run the detector but had nowhere to put the boxes.
#pragma once

#include <cstdint>
#include <string>

namespace camera::detect {

class IMetaSink {
public:
    virtual ~IMetaSink() = default;

    // `json` is one to_meta_json() payload for `camera`. Called from the
    // detection thread; implementations must not block it for long -- boxes
    // are best-effort and must never hold up the next frame.
    virtual void on_meta(uint8_t camera, const std::string& json) = 0;
};

}  // namespace camera::detect
