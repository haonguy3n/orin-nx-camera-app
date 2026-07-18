#pragma once
#include <QString>
#include <memory>
class QVideoSink;
class SecureUsbBridge {
public:
    static std::unique_ptr<SecureUsbBridge> start(QString *error);
    ~SecureUsbBridge();
    bool isRunning() const;
    // Where camera N's decoded frames are delivered (the pane's sink).
    // Frames arriving with no sink attached are dropped.
    void setVideoSink(int camera, QVideoSink *sink);
private:
    SecureUsbBridge() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
