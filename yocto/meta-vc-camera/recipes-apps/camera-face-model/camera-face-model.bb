SUMMARY = "YuNet face-detection model for camera-streamer"
DESCRIPTION = "Fetches and installs the YuNet ONNX model that camera-streamer's \
on-device face detection loads (see [detect] model in camera-streamer.conf). \
A separate recipe because the camera-streamer recipe uses externalsrc, which \
disables SRC_URI fetching."
# Model is OpenCV Zoo, MIT-licensed. Marked CLOSED to avoid a LIC_FILES_CHKSUM
# entry for a fetched blob; revisit for a clean license manifest.
LICENSE = "CLOSED"

# Auto-downloaded at build time (this recipe does not inherit externalsrc, so
# normal fetching works). GitHub's /raw/ resolves the LFS content directly.
# The sha256 pins exact content: if upstream ever changes this file the build
# fails loudly rather than shipping a different model.
SRC_URI = "https://github.com/opencv/opencv_zoo/raw/main/models/face_detection_yunet/face_detection_yunet_2023mar.onnx;downloadfilename=face_detection_yunet.onnx"
SRC_URI[sha256sum] = "8f2383e4dd3cfbb4553ea8718107fc0423210dc964f9f4280604804ed2552fa4"

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
