SUMMARY = "Face-detection model for camera-streamer"
DESCRIPTION = "Fetches and installs the face-detection ONNX that \
camera-streamer's on-device detection loads (see [detect] model in \
camera-streamer.conf). A separate recipe because the camera-streamer recipe \
uses externalsrc, which disables SRC_URI fetching."
# Model is OpenCV Zoo YuNet, MIT-licensed. Marked CLOSED to avoid a
# LIC_FILES_CHKSUM entry for a fetched blob; revisit for a clean manifest.
LICENSE = "CLOSED"

# Model source, overridable in local.conf. Default is the official OpenCV
# Hugging Face repo (a direct resolve/ URL -- steadier than GitHub LFS). Point
# these at any Hugging Face model to swap it, e.g.:
#   FACE_MODEL_URL = "https://huggingface.co/<org>/<repo>/resolve/<rev>/<file>.onnx"
#   FACE_MODEL_SHA256 = "<sha256 of that file>"
# A non-YuNet architecture also needs a matching IFaceDetector implementation
# (see embedded/src/camera/detect/) -- the loader interface is the extension
# point; the recipe only chooses the file.
FACE_MODEL_URL ?= "https://huggingface.co/opencv/face_detection_yunet/resolve/main/face_detection_yunet_2023mar.onnx"
FACE_MODEL_SHA256 ?= "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4"

# downloadfilename normalises the installed name regardless of the source URL,
# so the [detect] model path stays constant across model swaps.
SRC_URI = "${FACE_MODEL_URL};downloadfilename=face_detection_yunet.onnx"
SRC_URI[sha256sum] = "${FACE_MODEL_SHA256}"

S = "${WORKDIR}"

# Installed name matches the default [detect] model path in
# camera-streamer.conf: /usr/share/camera-streamer/face_detection_yunet.onnx
do_install() {
    install -d ${D}${datadir}/camera-streamer
    install -m 0644 ${WORKDIR}/face_detection_yunet.onnx \
        ${D}${datadir}/camera-streamer/face_detection_yunet.onnx
}

FILES:${PN} = "${datadir}/camera-streamer/face_detection_yunet.onnx"

# Arch-independent data.
inherit allarch
