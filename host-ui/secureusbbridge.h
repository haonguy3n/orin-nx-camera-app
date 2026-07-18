#pragma once
#include <QString>
#include <functional>
#include <memory>
#include <string>
class QVideoSink;
class SecureUsbBridge {
public:
    static std::unique_ptr<SecureUsbBridge> start(QString *error);
    ~SecureUsbBridge();
    bool isRunning() const;
    // Where camera N's decoded frames are delivered (the pane's sink).
    // Frames arriving with no sink attached are dropped.
    void setVideoSink(int camera, QVideoSink *sink);
    // Called on the bridge worker thread with each Channel::Meta payload
    // (face-box JSON, see SecureWire.h) as (camera, json). The handler must
    // marshal to the GUI thread itself.
    void setMetaHandler(std::function<void(int camera, std::string json)> handler);
private:
    SecureUsbBridge() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
