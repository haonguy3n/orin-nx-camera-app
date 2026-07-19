#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "camera/base/Expected.h"

namespace camera::detect {

// One detected face box, in the source frame's pixel coordinates.
struct FaceBox {
    int x = 0;
    int y = 0;
    int w = 0;
    int h = 0;
    float score = 0.0f;
};

// Serialise boxes to the Channel::Meta JSON payload (see SecureWire.h):
// {"camera":N,"w":W,"h":H,"faces":[{"x":,"y":,"w":,"h":,"score":}...]}.
std::string to_meta_json(uint8_t camera, int frame_width, int frame_height,
                         const std::vector<FaceBox>& boxes);

// Loader interface. Each implementation wraps one model/runtime (YuNet today;
// SCRFD/RetinaFace/a TensorRT engine can be added behind the same interface).
// Callers obtain one via create_face_detector and never see the concrete type.
//
// Not thread-safe: one detector per detection thread (one per camera).
class IFaceDetector {
public:
    virtual ~IFaceDetector() = default;

    // Detect faces in a packed BGRx image (4 bytes per pixel, as
    // nvvidconv emits). Returned boxes are scaled to
    // (frame_width, frame_height) so callers work in full-frame coordinates
    // regardless of the model's own input resolution.
    virtual std::vector<FaceBox> detect(const uint8_t* bgr, int frame_width,
                                        int frame_height, int stride) = 0;
};

// Chooses and loads a detector for `model_path` (currently YuNet via OpenCV's
// DNN CUDA backend). `input_width/height` is the model's working resolution.
// Fails if the model cannot be loaded.
[[nodiscard]] base::Expected<std::unique_ptr<IFaceDetector>, std::string>
create_face_detector(const std::string& model_path, int input_width,
                     int input_height, float score_threshold = 0.6f);

}  // namespace camera::detect
