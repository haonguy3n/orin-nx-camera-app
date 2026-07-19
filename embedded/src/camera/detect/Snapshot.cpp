#include "camera/detect/Snapshot.h"

#include <cstdio>

#include <array>
#include <mutex>
#include <string>
#include <vector>

namespace camera::detect {

namespace {

// One pending path per camera. A plain mutex-guarded slot rather than a queue:
// a snapshot request is "give me the next frame", so a second request before
// the first is served simply replaces it.
constexpr int kMaxCameras = 4;

std::mutex& slots_mutex() {
    static std::mutex m;
    return m;
}

std::array<std::string, kMaxCameras>& slots() {
    static std::array<std::string, kMaxCameras> s;
    return s;
}

}  // namespace

void request_snapshot(uint8_t camera, std::string path) {
    if (camera >= kMaxCameras) return;
    std::lock_guard<std::mutex> lock(slots_mutex());
    slots()[camera] = std::move(path);
}

std::string take_snapshot_request(uint8_t camera) {
    if (camera >= kMaxCameras) return {};
    std::lock_guard<std::mutex> lock(slots_mutex());
    std::string path;
    path.swap(slots()[camera]);
    return path;
}

std::string write_bgr_ppm(const std::string& path, const uint8_t* bgr,
                          int width, int height, int stride,
                          int bytes_per_pixel,
                          const std::vector<FaceBox>& boxes) {
    if (bgr == nullptr || width <= 0 || height <= 0)
        return "no frame to write";
    if (bytes_per_pixel != 3 && bytes_per_pixel != 4)
        return "unsupported pixel size";

    // Binary PPM (P6) rather than JPEG: cv::imwrite lives in
    // libopencv_imgcodecs, which is NOT in the device image, and pulling it in
    // would widen the image just to save a debug frame. PPM needs nothing but
    // fwrite, and it is lossless -- which matters here, because this image is
    // used to judge colour, and JPEG chroma subsampling would corrupt exactly
    // the thing being measured.
    std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3);
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = bgr + static_cast<size_t>(y) * stride;
        uint8_t* dst = rgb.data() + static_cast<size_t>(y) * width * 3;
        for (int x = 0; x < width; ++x) {
            const uint8_t* p = src + x * bytes_per_pixel;
            dst[x * 3 + 0] = p[2];  // R
            dst[x * 3 + 1] = p[1];  // G
            dst[x * 3 + 2] = p[0];  // B
        }
    }

    // Outline each detection in green, clipped to the frame.
    auto put = [&](int x, int y) {
        if (x < 0 || y < 0 || x >= width || y >= height) return;
        uint8_t* p = rgb.data() + (static_cast<size_t>(y) * width + x) * 3;
        p[0] = 0; p[1] = 255; p[2] = 0;
    };
    for (const FaceBox& b : boxes) {
        for (int t = 0; t < 2; ++t) {  // 2px so it survives downscaling
            for (int x = b.x; x < b.x + b.w; ++x) {
                put(x, b.y + t);
                put(x, b.y + b.h - 1 - t);
            }
            for (int y = b.y; y < b.y + b.h; ++y) {
                put(b.x + t, y);
                put(b.x + b.w - 1 - t, y);
            }
        }
    }

    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (f == nullptr) return "cannot open " + path;
    std::fprintf(f, "P6\n%d %d\n255\n", width, height);
    const size_t written = std::fwrite(rgb.data(), 1, rgb.size(), f);
    const bool ok = written == rgb.size();
    std::fclose(f);
    if (!ok) return "short write to " + path;
    return {};
}

}  // namespace camera::detect
