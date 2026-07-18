# YuNet face-detection model

`camera-face-model.bb` ships the YuNet ONNX that camera-streamer loads for
on-device face detection. The model is a binary blob and is **not** committed
to this repo — download it into this directory before building.

## Get the model

From the OpenCV Zoo (MIT-licensed), pin a known revision for reproducibility:

```sh
cd yocto/meta-vc-camera/recipes-apps/camera-face-model/files
curl -L -o face_detection_yunet.onnx \
  https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx
```

The 2023mar variant matches the `cv::FaceDetectorYN` API used in
`embedded/src/camera/detect/FaceDetector.cpp`. It runs at the detector working
resolution set in `[detect] width/height` (default 320x320).

## Enable it

1. Build with the model included (in `local.conf`):
   ```
   CAMERA_FACE_MODEL = "1"
   ```
2. On the device, turn detection on in `/etc/camera-streamer.conf`:
   ```
   [detect]
   enabled=true
   model=/usr/share/camera-streamer/face_detection_yunet.onnx
   ```
   then `systemctl restart camera-streamer`.

Boxes are emitted over the secure-USB metadata channel and drawn by the viewer.
