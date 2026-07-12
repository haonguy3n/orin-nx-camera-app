#include "pipeline/source_factory.h"

#include "pipeline/argus_source.h"
#include "pipeline/test_source.h"
#include "pipeline/v4l2_source.h"

std::unique_ptr<ICameraSource> SourceFactory::create(
    const std::string& source_type) {
    if (source_type == "argus")
        return std::make_unique<ArgusSource>();
    if (source_type == "v4l2")
        return std::make_unique<V4l2Source>(v4l2_factory_);
    if (source_type == "test")
        return std::make_unique<TestSource>();
    return nullptr;
}
