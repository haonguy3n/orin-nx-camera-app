# Face-detection model

`camera-face-model.bb` ships the face-detection ONNX that camera-streamer
loads for on-device detection. The recipe **downloads it automatically** at
build time — nothing to place here by hand. The default is the official OpenCV
YuNet, hosted on **Hugging Face** (a direct `resolve/` URL, steadier than
GitHub LFS), sha256-pinned.

## Always shipped

The image always installs `camera-face-model` (see camera-image.bb) to
`/usr/share/camera-streamer/face_detection_yunet.onnx`. camera-streamer
auto-enables detection whenever that file is present — no config switch. To
turn it off, point `[detect] model=` at a nonexistent path.

## Use a different Hugging Face model

Override the source in `local.conf`:

```
FACE_MODEL_URL = "https://huggingface.co/<org>/<repo>/resolve/<rev>/<file>.onnx"
FACE_MODEL_SHA256 = "<sha256 of that file>"
```

(Get the sha256 with `sha256sum` on a local copy, or run the build once and
copy the value bitbake reports on the mismatch.)

- **YuNet-format ONNX** (any mirror/version) → works as-is; only the URL+sha
  change.
- **A different architecture** (SCRFD, RetinaFace, YOLO-face, …) → also needs a
  new `IFaceDetector` implementation with that model's pre/post-processing in
  `embedded/src/camera/detect/` and a dispatch line in `create_face_detector`.
  The loader interface is the extension point; the recipe only picks the file.
- Must be **ONNX** — OpenCV DNN cannot load PyTorch/safetensors checkpoints.

Boxes are emitted over the secure-USB metadata channel and drawn by the viewer.
