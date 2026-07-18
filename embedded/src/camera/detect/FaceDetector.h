#pragma once

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

// GPU face detector: OpenCV's YuNet (cv::FaceDetectorYN) run through the DNN
// module's CUDA backend (which uses cuDNN), so inference lands on the Orin's
// GPU next to the encoders. Kept behind this interface so a TensorRT engine
// can replace the implementation later without touching callers.
//
// Not thread-safe: one detector per detection thread (one per camera).
class FaceDetector {
public:
    // `model_path` is the YuNet .onnx; `input` is the detector's working
    // resolution (frames are letterbox-free resized to it). Fails if the
    // model cannot be loaded.
    [[nodiscard]] static base::Expected<std::unique_ptr<FaceDetector>, std::string>
    create(const std::string& model_path, int input_width, int input_height,
           float score_threshold = 0.6f);
    ~FaceDetector();

    FaceDetector(const FaceDetector&) = delete;
    FaceDetector& operator=(const FaceDetector&) = delete;

    // Detect faces in a packed BGR image of the given size. Returned boxes are
    // scaled back to (frame_width, frame_height) so callers work in full-res
    // coordinates regardless of the detector's input resolution.
    std::vector<FaceBox> detect(const uint8_t* bgr, int frame_width,
                                int frame_height, int stride);

private:
    struct Impl;
    explicit FaceDetector(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace camera::detect
