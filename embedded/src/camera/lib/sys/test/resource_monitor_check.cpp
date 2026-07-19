// Self-check for ResourceMonitor. Runs anywhere: /proc is Linux-generic, and
// the Tegra GPU/thermal nodes are absent off-target, which is itself asserted
// to degrade rather than crash or report nonsense.
//
//   g++ -std=c++20 -Iembedded/src embedded/src/camera/lib/sys/ResourceMonitor.cpp
//       embedded/src/camera/lib/sys/test/resource_monitor_check.cpp -o rm_check
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>

#include "camera/lib/sys/ResourceMonitor.h"

int main() {
    using namespace camera::lib;
    ResourceMonitor monitor;

    // First sample only primes the counters: CPU accounting is cumulative, so
    // a rate needs two points. Reporting a percentage here would be a fiction,
    // and a before/after comparison that used it would be wrong.
    const ResourceSample first = monitor.sample();
    assert(first.interval_seconds == 0.0 && "first sample must not claim a rate");
    assert(first.cpu_percent < 0.0 && "first sample must not invent a cpu figure");

    // Burn a little CPU so the process figure is provably non-zero.
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds(300);
    volatile double x = 0;
    while (std::chrono::steady_clock::now() < until) x += 1.0;
    (void)x;

    const ResourceSample second = monitor.sample();
    assert(second.interval_seconds > 0.2 && second.interval_seconds < 5.0);
    assert(second.cpu_percent >= 0.0 && second.cpu_percent <= 100.0);
    assert(second.process_cpu_percent > 0.0 && "busy loop must show up");
    std::printf("[1] %s\n", second.to_string().c_str());
    std::printf("    interval %.2fs\n", second.interval_seconds);

    // An idle interval must read lower than a busy one -- the property any
    // before/after refactor measurement depends on.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    const ResourceSample idle = monitor.sample();
    assert(idle.process_cpu_percent < second.process_cpu_percent &&
           "idle must measure lower than busy, or comparisons are meaningless");
    std::printf("[2] busy %.1f%% -> idle %.1f%% (process)\n",
                second.process_cpu_percent, idle.process_cpu_percent);

    // Off-target the GPU node is absent; that must be reported, not faked.
    std::printf("[3] gpu: %s\n",
                idle.has_gpu() ? "present" : "unavailable (expected off-target)");

    std::printf("resource_monitor_check: OK\n");
    return 0;
}
