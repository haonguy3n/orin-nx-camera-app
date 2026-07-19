// CPU/GPU/thermal sampling, so a refactor's cost can be measured instead of
// argued about.
//
// Written because the detect branch turned out to hold VIC ~70% and GR3D ~40%
// continuously for frames nobody looked at -- found only by running tegrastats
// by hand over ssh. That number should be observable from the device itself,
// before and after a change.
//
// Reads /proc and sysfs directly rather than shelling out to tegrastats: no
// fork from a multi-threaded process (which is how the video reader deadlocked
// once), no parsing of a human-facing format, and it works on a dev host too
// (GPU/thermal simply report unavailable).
#pragma once

#include <cstdint>
#include <string>

namespace camera::lib {

// A measurement over the interval since the previous sample().
struct ResourceSample {
    // Busy percentage across all cores, 0..100. Negative if unavailable.
    double cpu_percent = -1.0;
    // This process's CPU, 0..100 per core-equivalent (may exceed 100 with
    // several busy threads -- video, detection and the endpoint writer are
    // separate threads, so that is expected and informative).
    double process_cpu_percent = -1.0;
    // Tegra GPU (GR3D) load 0..100. Negative if the sysfs node is absent,
    // which is the normal case off-target.
    double gpu_percent = -1.0;
    // VIC (video image compositor) and NVENC. VIC measured ~70% with the
    // detect branch absent entirely, i.e. it is the camera/encode path --
    // reported separately so it is not blamed on detection again.
    double vic_percent = -1.0;
    double nvenc_percent = -1.0;
    double cpu_temp_c = -1.0;
    double gpu_temp_c = -1.0;
    // Interval this sample covers. 0 for the first call, which only primes
    // the counters -- percentages are meaningless without a previous point.
    double interval_seconds = 0.0;

    [[nodiscard]] bool has_gpu() const { return gpu_percent >= 0.0; }
    // One-line form for logs and get-metrics.
    [[nodiscard]] std::string to_string() const;
};

class ResourceMonitor {
public:
    ResourceMonitor();

    // Samples and returns the deltas since the previous call. The FIRST call
    // returns interval_seconds == 0 and no percentages: CPU accounting is
    // cumulative, so a rate needs two points. Callers measuring a change
    // should discard it.
    ResourceSample sample();

private:
    // Cumulative jiffies from /proc/stat and /proc/self/stat.
    uint64_t total_ = 0, idle_ = 0, process_ = 0;
    uint64_t last_ns_ = 0;
    bool primed_ = false;
};

}  // namespace camera::lib
