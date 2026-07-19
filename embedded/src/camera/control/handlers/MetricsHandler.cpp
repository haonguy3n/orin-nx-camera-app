#include "camera/control/handlers/MetricsHandler.h"

#include "camera/control/JsonUtil.h"
#include "camera/lib/sys/ResourceMonitor.h"

namespace camera {

HandlerResult GetMetricsHandler::handle(JsonObject* /*params*/,
                                        ControlContext& /*ctx*/) {
    // One monitor for the process: percentages are deltas since the previous
    // call, so state must persist across requests. The FIRST call after start
    // only primes the counters and reports negatives -- poll twice and use the
    // second, which is why interval_s is reported rather than hidden.
    static lib::ResourceMonitor monitor;
    const lib::ResourceSample s = monitor.sample();

    JsonBuilder* b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "cpu_percent");
    json_builder_add_double_value(b, s.cpu_percent);
    json_builder_set_member_name(b, "process_cpu_percent");
    json_builder_add_double_value(b, s.process_cpu_percent);
    json_builder_set_member_name(b, "gpu_percent");
    json_builder_add_double_value(b, s.gpu_percent);
    json_builder_set_member_name(b, "vic_percent");
    json_builder_add_double_value(b, s.vic_percent);
    json_builder_set_member_name(b, "nvenc_percent");
    json_builder_add_double_value(b, s.nvenc_percent);
    json_builder_set_member_name(b, "cpu_temp_c");
    json_builder_add_double_value(b, s.cpu_temp_c);
    json_builder_set_member_name(b, "gpu_temp_c");
    json_builder_add_double_value(b, s.gpu_temp_c);
    json_builder_set_member_name(b, "interval_s");
    json_builder_add_double_value(b, s.interval_seconds);
    json_builder_end_object(b);
    return take_root(b);
}

}  // namespace camera
