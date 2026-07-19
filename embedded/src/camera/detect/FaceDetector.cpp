#include "camera/detect/FaceDetector.h"

#include <cstdio>

#include <opencv2/core.hpp>
#include <opencv2/dnn.hpp>  // cv::dnn::DNN_BACKEND_CUDA / DNN_TARGET_CUDA
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

namespace {

// YuNet (cv::FaceDetectorYN) implementation of the loader interface, run
// through the DNN CUDA backend (cuDNN) so inference lands on the Orin GPU.
class YuNetDetector : public IFaceDetector {
public:
    YuNetDetector(cv::Ptr<cv::FaceDetectorYN> yunet, cv::Size input)
        : yunet_(std::move(yunet)), input_(input) {}

    std::vector<FaceBox> detect(const uint8_t* bgr, int frame_width,
                                int frame_height, int stride) override {
        std::vector<FaceBox> boxes;
        try {
            const cv::Mat frame(frame_height, frame_width, CV_8UC3,
                                const_cast<uint8_t*>(bgr), static_cast<size_t>(stride));
            // Detect at the frame's own size rather than resizing to a fixed
            // input. The pipeline already scales to the detector's working
            // resolution preserving the camera's aspect ratio, so resizing
            // again to a square here would re-introduce exactly the distortion
            // that made YuNet miss faces -- and cost a copy per frame.
            const cv::Size size(frame_width, frame_height);
            if (size != input_) {
                yunet_->setInputSize(size);
                input_ = size;
            }
            cv::Mat faces;
            yunet_->detect(frame, faces);

            // Boxes come back in frame coordinates already.
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

private:
    cv::Ptr<cv::FaceDetectorYN> yunet_;
    cv::Size input_;
};

}  // namespace

base::Expected<std::unique_ptr<IFaceDetector>, std::string> create_face_detector(
    const std::string& model_path, int input_width, int input_height,
    float score_threshold) {
    // Only YuNet today. When a second model is added, dispatch here on the
    // model kind (e.g. a filename convention or a config field) and construct
    // the matching IFaceDetector.
    try {
        const cv::Size input(input_width, input_height);
        auto yunet = cv::FaceDetectorYN::create(
            model_path, /*config=*/"", input, score_threshold,
            /*nms_threshold=*/0.3f, /*top_k=*/50,
            cv::dnn::DNN_BACKEND_CUDA, cv::dnn::DNN_TARGET_CUDA);
        if (yunet.empty())
            return base::makeUnexpected(std::string("YuNet model failed to load: ") + model_path);
        return std::unique_ptr<IFaceDetector>(new YuNetDetector(std::move(yunet), input));
    } catch (const cv::Exception& e) {
        return base::makeUnexpected(std::string("face detector init: ") + e.what());
    }
}

}  // namespace camera::detect
