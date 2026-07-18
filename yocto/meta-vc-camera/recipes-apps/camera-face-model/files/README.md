# YuNet face-detection model

`camera-face-model.bb` ships the YuNet ONNX that camera-streamer loads for
on-device face detection. The recipe **downloads it automatically** at build
time (OpenCV Zoo, MIT-licensed, sha256-pinned) — nothing to place here by hand.

## Always shipped

The image always installs `camera-face-model` (see camera-image.bb), which
fetches `face_detection_yunet_2023mar.onnx` at build time and installs it to
`/usr/share/camera-streamer/face_detection_yunet.onnx`. camera-streamer
auto-enables detection whenever that file is present -- no config switch. To
turn it off, point `[detect] model=` at a nonexistent path (or remove the
model from the image).

Boxes are emitted over the secure-USB metadata channel and drawn by the viewer.

## Updating the model

If upstream changes the file, the build fails on a sha256 mismatch. Update
`SRC_URI[sha256sum]` in the recipe to the new value (bitbake prints it), or pin
`SRC_URI` to a specific opencv_zoo commit for full reproducibility.
