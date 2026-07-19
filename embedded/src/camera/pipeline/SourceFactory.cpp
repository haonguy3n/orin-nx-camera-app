#include "camera/pipeline/SourceFactory.h"

#include "camera/pipeline/ArgusSource.h"
#include "camera/pipeline/TestSource.h"
#include "camera/pipeline/V4l2Source.h"

namespace camera {

std::unique_ptr<ICameraSource> create_source(const std::string& source_type) {
    if (source_type == "argus")
        return std::make_unique<ArgusSource>();
    if (source_type == "v4l2")
        return std::make_unique<V4l2Source>();
    if (source_type == "test")
        return std::make_unique<TestSource>();
    return nullptr;
}

}  // namespace camera
