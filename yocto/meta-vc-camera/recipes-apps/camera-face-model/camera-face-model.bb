SUMMARY = "YuNet face-detection model for camera-streamer"
DESCRIPTION = "Ships the YuNet ONNX model that camera-streamer's on-device \
face detection loads (see [detect] model in camera-streamer.conf). Kept as a \
separate recipe because the camera-streamer recipe uses externalsrc, which \
disables SRC_URI fetching."
# The model is redistributed under its own upstream terms (OpenCV Zoo, MIT).
# Marked CLOSED here to avoid a checksum entry for a vendored blob; revisit if
# the image needs a clean license manifest.
LICENSE = "CLOSED"

# The ONNX is not committed to this repo (binary blob). Drop it into files/
# next to this recipe -- see files/README.md for the download. A build that
# pulls this recipe in without the model fails at fetch with a clear
# "file not found", which is the intended signal.
SRC_URI = "file://face_detection_yunet.onnx"

S = "${WORKDIR}"

# Model name matches the default [detect] model path in camera-streamer.conf:
#   /usr/share/camera-streamer/face_detection_yunet.onnx
do_install() {
    install -d ${D}${datadir}/camera-streamer
    install -m 0644 ${WORKDIR}/face_detection_yunet.onnx \
        ${D}${datadir}/camera-streamer/face_detection_yunet.onnx
}

FILES:${PN} = "${datadir}/camera-streamer/face_detection_yunet.onnx"

# Arch-independent data.
inherit allarch
