// On-demand frame grab for ISP debugging.
//
// Tuning the ISP means looking at what the sensor actually produces, and on
// this device that is awkward: Argus allows one consumer per camera, so the
// running service holds the sensor and nothing else can open it. Stopping
// camera-streamer to free it also unbinds the USB gadget -- taking the CDC-NCM
// link (and any ssh session) down with it.
//
// So the snapshot is taken from inside the running pipeline instead: the face
// detection branch already carries post-ISP BGR frames, and one of them is
// written out on request. No extra tee branch, no pipeline change, no restart.
//
// Resolution is the detector's working size (see [detect] width), not full
// sensor resolution. That is deliberate and sufficient for the colour-domain
// problems ISP tuning is about -- black pedestal, white balance, colour casts,
// saturation -- which are all measurable at this size. It is NOT useful for
// judging sharpness or noise.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "camera/detect/FaceDetector.h"

namespace camera::detect {

// Requests that the next frame from `camera`'s detection branch be written to
// `path`. Overwrites any pending request for that camera. Thread-safe.
void request_snapshot(uint8_t camera, std::string path);

// Claims a pending request for `camera`, if any, clearing it. Returns an empty
// string when nothing is pending. Thread-safe.
std::string take_snapshot_request(uint8_t camera);

// Writes a packed BGR frame as a binary PPM (P6). Returns an error string on
// failure, empty on success. PPM because libopencv_imgcodecs is not in the
// device image, and because it is lossless -- this frame is used to judge
// colour, which JPEG chroma subsampling would distort.
// `bytes_per_pixel` is 4 for the BGRx nvvidconv emits (the padding byte is
// skipped) or 3 for packed BGR.
// `boxes` are outlined in the written image. They come from the very frame
// being written, so a snapshot answers "is detection finding anything, and
// where" directly -- rather than inferring it from GPU load or a log count.
std::string write_bgr_ppm(const std::string& path, const uint8_t* bgr,
                          int width, int height, int stride,
                          int bytes_per_pixel,
                          const std::vector<FaceBox>& boxes = {});

}  // namespace camera::detect
