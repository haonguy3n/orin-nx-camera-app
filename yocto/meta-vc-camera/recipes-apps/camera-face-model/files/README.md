# YuNet face-detection model

`camera-face-model.bb` ships the YuNet ONNX that camera-streamer loads for
on-device face detection. The recipe **downloads it automatically** at build
time (OpenCV Zoo, MIT-licensed, sha256-pinned) — nothing to place here by hand.

## Enable it

1. Build with the model included (in `local.conf`):
   ```
   CAMERA_FACE_MODEL = "1"
   ```
   The image then pulls in `camera-face-model`, which fetches
   `face_detection_yunet_2023mar.onnx` and installs it to
   `/usr/share/camera-streamer/face_detection_yunet.onnx`.
2. On the device, turn detection on in `/etc/camera-streamer.conf`:
   ```
   [detect]
   enabled=true
   model=/usr/share/camera-streamer/face_detection_yunet.onnx
   ```
   then `systemctl restart camera-streamer`.

Boxes are emitted over the secure-USB metadata channel and drawn by the viewer.

## Updating the model

If upstream changes the file, the build fails on a sha256 mismatch. Update
`SRC_URI[sha256sum]` in the recipe to the new value (bitbake prints it), or pin
`SRC_URI` to a specific opencv_zoo commit for full reproducibility.
