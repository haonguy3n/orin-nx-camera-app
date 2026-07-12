// Camera source factory (Factory pattern).
//
// Creates the appropriate ICameraSource strategy from a source type
// string. This decouples the rest of the code from the concrete source
// classes and enables testing with mock sources.
#pragma once

#include <memory>

#include "pipeline/camera_source.h"
#include "v4l2/v4l2_device.h"

class ISourceFactory {
public:
    virtual ~ISourceFactory() = default;
    // Creates a source strategy for |source_type| ("argus", "v4l2", "test").
    // Returns nullptr for unknown types.
    virtual std::unique_ptr<ICameraSource> create(
        const std::string& source_type) = 0;
};

// Production factory: creates real ArgusSource, V4l2Source, TestSource.
class SourceFactory : public ISourceFactory {
public:
    explicit SourceFactory(IV4l2DeviceFactory& v4l2_factory)
        : v4l2_factory_(v4l2_factory) {}

    std::unique_ptr<ICameraSource> create(
        const std::string& source_type) override;

private:
    IV4l2DeviceFactory& v4l2_factory_;
};
