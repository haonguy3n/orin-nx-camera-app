// Self-check for the ISP snapshot writer. Runs anywhere (no OpenCV, no Argus):
//   g++ -std=c++20 -Iembedded/src embedded/src/camera/detect/Snapshot.cpp
//       embedded/src/camera/detect/test/snapshot_check.cpp -o snapshot_check
//
// Guards the channel order specifically. The detect branch delivers BGR and
// PPM stores RGB, so a swapped write would tint every snapshot -- and since
// these snapshots exist to judge ISP colour, the error would be read as a
// camera white-balance fault and chased in the wrong place entirely.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "camera/detect/Snapshot.h"

int main() {
    using namespace camera::detect;

    // 2x2 BGR with a deliberately padded stride, so stride handling is
    // exercised rather than assumed to equal width*3.
    const int w = 2, h = 2, stride = 10;  // 6 bytes of pixels + 4 padding
    std::vector<uint8_t> bgr(static_cast<size_t>(stride) * h, 0xEE);  // padding
    auto px = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r) {
        uint8_t* p = bgr.data() + y * stride + x * 3;
        p[0] = b; p[1] = g; p[2] = r;
    };
    px(0, 0, 255, 0, 0);    // pure blue
    px(1, 0, 0, 255, 0);    // pure green
    px(0, 1, 0, 0, 255);    // pure red
    px(1, 1, 10, 20, 30);   // mixed

    const std::string path = "/tmp/snapshot_check.ppm";
    std::remove(path.c_str());
    const std::string failure =
        write_bgr_ppm(path, bgr.data(), w, h, stride, /*bytes_per_pixel=*/3);
    assert(failure.empty() && "write_bgr_ppm reported failure");

    std::FILE* f = std::fopen(path.c_str(), "rb");
    assert(f != nullptr && "snapshot file was not created");
    char header[16] = {};
    int hw = 0, hh = 0, maxval = 0;
    assert(std::fscanf(f, "%2s %d %d %d", header, &hw, &hh, &maxval) == 4);
    assert(std::string(header) == "P6");
    assert(hw == w && hh == h && maxval == 255);
    std::fgetc(f);  // single whitespace byte before the raster

    std::vector<uint8_t> rgb(static_cast<size_t>(w) * h * 3);
    assert(std::fread(rgb.data(), 1, rgb.size(), f) == rgb.size());
    // Nothing past the raster: padding must not leak into the file.
    assert(std::fgetc(f) == EOF && "padding bytes leaked into the raster");
    std::fclose(f);

    // BGR in -> RGB out, and the stride padding (0xEE) must appear nowhere.
    assert(rgb[0] == 0 && rgb[1] == 0 && rgb[2] == 255);      // pure blue
    assert(rgb[3] == 0 && rgb[4] == 255 && rgb[5] == 0);      // pure green
    assert(rgb[6] == 255 && rgb[7] == 0 && rgb[8] == 0);      // pure red
    assert(rgb[9] == 30 && rgb[10] == 20 && rgb[11] == 10);   // mixed, swapped
    for (uint8_t byte : rgb) assert(byte != 0xEE);

    std::remove(path.c_str());

    // BGRx (4 bytes/pixel) is what nvvidconv actually delivers: same colours,
    // plus a padding byte that must be skipped rather than shifted into the
    // next pixel -- which would smear the whole image one channel sideways.
    const int stride4 = 4 * 2 + 5;  // 2 px of BGRx + odd padding
    std::vector<uint8_t> bgrx(static_cast<size_t>(stride4) * h, 0xEE);
    auto px4 = [&](int x, int y, uint8_t b, uint8_t g, uint8_t r) {
        uint8_t* p = bgrx.data() + y * stride4 + x * 4;
        p[0] = b; p[1] = g; p[2] = r; p[3] = 0x7F;  // x byte, must be ignored
    };
    px4(0, 0, 255, 0, 0);
    px4(1, 0, 0, 255, 0);
    px4(0, 1, 0, 0, 255);
    px4(1, 1, 10, 20, 30);
    assert(write_bgr_ppm(path, bgrx.data(), w, h, stride4, 4).empty());
    f = std::fopen(path.c_str(), "rb");
    assert(f != nullptr);
    assert(std::fscanf(f, "%2s %d %d %d", header, &hw, &hh, &maxval) == 4);
    std::fgetc(f);
    std::vector<uint8_t> rgb4(static_cast<size_t>(w) * h * 3);
    assert(std::fread(rgb4.data(), 1, rgb4.size(), f) == rgb4.size());
    std::fclose(f);
    assert(rgb4 == rgb && "BGRx must yield the same RGB as BGR");
    for (uint8_t byte : rgb4) assert(byte != 0x7F && byte != 0xEE);

    // A pixel size the writer cannot interpret must be refused, not guessed.
    assert(!write_bgr_ppm(path, bgrx.data(), w, h, stride4, 2).empty());

    std::remove(path.c_str());
    std::printf("snapshot_check: OK (BGR and BGRx)\n");
    return 0;
}
