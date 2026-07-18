#include "camera/detect/FaceDetector.h"

#include <cstdio>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect/face.hpp>

namespace camera::detect {

std::string to_meta_json(uint8_t camera, int frame_width, int frame_height,
                         const std::vector<FaceBox>& boxes) {
    std::string out = "{\"camera\":" + std::to_string(camera)
        + ",\"w\":" + std::to_string(frame_width)
        + ",\"h\":" + std::to_string(frame_height) + ",\"faces\":[";
    for (size_t i = 0; i < boxes.size(); ++i) {
        char score[16];
        std::snprintf(score, sizeof(score), "%.3f", boxes[i].score);
        if (i != 0) out += ',';
        out += "{\"x\":" + std::to_string(boxes[i].x)
             + ",\"y\":" + std::to_string(boxes[i].y)
             + ",\"w\":" + std::to_string(boxes[i].w)
             + ",\"h\":" + std::to_string(boxes[i].h)
             + ",\"score\":" + score + '}';
    }
    out += "]}";
    return out;
}

struct FaceDetector::Impl {
    cv::Ptr<cv::FaceDetectorYN> yunet;
    cv::Size input;
};

FaceDetector::FaceDetector(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FaceDetector::~FaceDetector() = default;

base::Expected<std::unique_ptr<FaceDetector>, std::string> FaceDetector::create(
    const std::string& model_path, int input_width, int input_height,
    float score_threshold) {
    try {
        auto impl = std::make_unique<Impl>();
        impl->input = cv::Size(input_width, input_height);
        // Backend/target CUDA: OpenCV routes YuNet through the DNN CUDA
        // backend (cuDNN), so inference runs on the Orin GPU. An OpenCV built
        // without CUDA falls back to the CPU backend transparently.
        impl->yunet = cv::FaceDetectorYN::create(
            model_path, /*config=*/"", impl->input, score_threshold,
            /*nms_threshold=*/0.3f, /*top_k=*/50,
            cv::dnn::DNN_BACKEND_CUDA, cv::dnn::DNN_TARGET_CUDA);
        if (impl->yunet.empty())
            return base::makeUnexpected(std::string("YuNet model failed to load: ") + model_path);
        return std::unique_ptr<FaceDetector>(new FaceDetector(std::move(impl)));
    } catch (const cv::Exception& e) {
        return base::makeUnexpected(std::string("FaceDetector init: ") + e.what());
    }
}

std::vector<FaceBox> FaceDetector::detect(const uint8_t* bgr, int frame_width,
                                          int frame_height, int stride) {
    std::vector<FaceBox> boxes;
    try {
        const cv::Mat frame(frame_height, frame_width, CV_8UC3,
                            const_cast<uint8_t*>(bgr), static_cast<size_t>(stride));
        cv::Mat resized;
        cv::resize(frame, resized, impl_->input);
        // setInputSize must match the image handed to detect().
        impl_->yunet->setInputSize(impl_->input);
        cv::Mat faces;
        impl_->yunet->detect(resized, faces);

        // Scale detector-space boxes back to the source frame.
        const float sx = static_cast<float>(frame_width) / impl_->input.width;
        const float sy = static_cast<float>(frame_height) / impl_->input.height;
        boxes.reserve(faces.rows);
        for (int i = 0; i < faces.rows; ++i) {
            // Row layout: x, y, w, h, [5 landmarks], score.
            const float* row = faces.ptr<float>(i);
            FaceBox box;
            box.x = static_cast<int>(row[0] * sx);
            box.y = static_cast<int>(row[1] * sy);
            box.w = static_cast<int>(row[2] * sx);
            box.h = static_cast<int>(row[3] * sy);
            box.score = row[faces.cols - 1];
            boxes.push_back(box);
        }
    } catch (const cv::Exception&) {
        // A transient decode/format hiccup should not kill the detection
        // thread; the next frame gets a fresh attempt.
        boxes.clear();
    }
    return boxes;
}

}  // namespace camera::detect
