#include "camera/lib/sys/ResourceMonitor.h"

#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace camera::lib {

namespace {

uint64_t now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL +
           static_cast<uint64_t>(ts.tv_nsec);
}

std::string read_file(const char* path) {
    std::FILE* f = std::fopen(path, "re");
    if (f == nullptr) return {};
    char buffer[512];
    const size_t n = std::fread(buffer, 1, sizeof(buffer) - 1, f);
    std::fclose(f);
    buffer[n] = '\0';
    return std::string(buffer, n);
}

// Aggregate jiffies from /proc/stat's first line: total and idle+iowait.
bool read_cpu(uint64_t* total, uint64_t* idle) {
    const std::string stat = read_file("/proc/stat");
    if (stat.compare(0, 4, "cpu ") != 0) return false;
    uint64_t v[10] = {};
    const int got = std::sscanf(
        stat.c_str() + 4, "%lu %lu %lu %lu %lu %lu %lu %lu %lu %lu", &v[0],
        &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    if (got < 5) return false;
    *total = 0;
    for (int i = 0; i < got; ++i) *total += v[i];
    *idle = v[3] + v[4];  // idle + iowait
    return true;
}

// utime+stime from /proc/self/stat. Fields 14/15, after the comm field, which
// can itself contain spaces and parentheses -- hence the rfind(')').
bool read_process(uint64_t* jiffies) {
    const std::string stat = read_file("/proc/self/stat");
    const size_t close = stat.rfind(')');
    if (close == std::string::npos) return false;
    uint64_t utime = 0, stime = 0;
    // After ") S" come the numeric fields; utime is the 12th of them.
    if (std::sscanf(stat.c_str() + close + 2,
                    "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu",
                    &utime, &stime) != 2)
        return false;
    *jiffies = utime + stime;
    return true;
}

// Tegra GPU load, per-mille in sysfs. The node moved between L4T releases, so
// try the known ones and report unavailable rather than guessing wrong.
double read_gpu_percent() {
    // Orin (L4T 35.x, ga10b) first -- verified on target reading 366 while
    // tegrastats showed 37%. The older gpu.0 paths are kept for other Tegras.
    static const char* const kCandidates[] = {
        "/sys/devices/platform/17000000.ga10b/load",
        "/sys/class/devfreq/17000000.ga10b/device/load",
        "/sys/devices/platform/gpu.0/load",
        "/sys/devices/gpu.0/load",
    };
    for (const char* path : kCandidates) {
        const std::string value = read_file(path);
        if (value.empty()) continue;
        try {
            return std::stod(value) / 10.0;  // per-mille -> percent
        } catch (...) {
            continue;
        }
    }
    return -1.0;
}

// Thermal zone whose type contains `needle` (e.g. "CPU", "GPU").
// devfreq load for a named engine (per-mille). VIC and NVENC matter here: VIC
// sat at ~70% even with the detect branch entirely absent, so attributing it
// to detection was a mistake worth making impossible to repeat.
double read_devfreq_load(const char* device) {
    char path[160];
    std::snprintf(path, sizeof(path), "/sys/class/devfreq/%s/device/load", device);
    const std::string value = read_file(path);
    if (value.empty()) return -1.0;
    try {
        return std::stod(value) / 10.0;
    } catch (...) {
        return -1.0;
    }
}

double read_temp_c(const char* needle) {
    for (int zone = 0; zone < 16; ++zone) {
        char path[128];
        std::snprintf(path, sizeof(path),
                      "/sys/devices/virtual/thermal/thermal_zone%d/type", zone);
        const std::string type = read_file(path);
        if (type.empty()) continue;
        if (type.find(needle) == std::string::npos) continue;
        std::snprintf(path, sizeof(path),
                      "/sys/devices/virtual/thermal/thermal_zone%d/temp", zone);
        const std::string value = read_file(path);
        if (value.empty()) continue;
        try {
            return std::stod(value) / 1000.0;  // millidegrees
        } catch (...) {
            continue;
        }
    }
    return -1.0;
}

}  // namespace

std::string ResourceSample::to_string() const {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "cpu %.1f%% (proc %.1f%%) gpu %s cpu_temp %.1fC gpu_temp %.1fC",
                  cpu_percent, process_cpu_percent,
                  has_gpu() ? (std::to_string(static_cast<int>(gpu_percent)) +
                               "%").c_str()
                            : "n/a",
                  cpu_temp_c, gpu_temp_c);
    return buffer;
}

ResourceMonitor::ResourceMonitor() = default;

ResourceSample ResourceMonitor::sample() {
    ResourceSample out;
    uint64_t total = 0, idle = 0, process = 0;
    const bool have_cpu = read_cpu(&total, &idle);
    const bool have_process = read_process(&process);
    const uint64_t now = now_ns();

    if (primed_ && have_cpu) {
        const uint64_t d_total = total - total_;
        const uint64_t d_idle = idle - idle_;
        if (d_total > 0) {
            out.cpu_percent =
                100.0 * static_cast<double>(d_total - d_idle) / d_total;
        }
        out.interval_seconds = static_cast<double>(now - last_ns_) / 1e9;
        if (have_process && out.interval_seconds > 0) {
            const long hz = sysconf(_SC_CLK_TCK);
            const double seconds =
                hz > 0 ? static_cast<double>(process - process_) / hz : 0.0;
            out.process_cpu_percent = 100.0 * seconds / out.interval_seconds;
        }
    }

    if (have_cpu) {
        total_ = total;
        idle_ = idle;
    }
    if (have_process) process_ = process;
    last_ns_ = now;
    primed_ = have_cpu;

    out.gpu_percent = read_gpu_percent();
    out.vic_percent = read_devfreq_load("15340000.vic");
    out.nvenc_percent = read_devfreq_load("154c0000.nvenc");
    out.cpu_temp_c = read_temp_c("CPU");
    out.gpu_temp_c = read_temp_c("GPU");
    return out;
}

}  // namespace camera::lib
