#pragma once
// =============================================================================
// SystemMonitor.h — This process's CPU, memory and GPU usage, sampled at 1 Hz
// =============================================================================
// CPU:    GetProcessTimes delta / wall time / logical cores (% of the machine,
//         same scale as Task Manager's Processes tab)
// Memory: working set and private bytes (GetProcessMemoryInfo)
// GPU:    PDH "\GPU Engine(*)\Utilization Percentage" filtered to our PID,
//         summed per engine type (3D, VideoDecode) — the counters Task Manager
//         uses. Requires WDDM 2.x drivers; reported as unavailable otherwise.
//
// Runs on its own thread: collecting GPU counters takes a few milliseconds and
// must never stall the render loop.
// =============================================================================

#include "Performance/PipelineMetrics.h"   // SystemSample

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

class SystemMonitor {
public:
    SystemMonitor();
    ~SystemMonitor();
    SystemMonitor(const SystemMonitor&) = delete;
    SystemMonitor& operator=(const SystemMonitor&) = delete;

    /// Latest sample (valid after the first second)
    SystemSample latest() const;

private:
    void run();

    std::thread thread_;
    std::atomic<bool> running_{true};
    std::mutex wakeMutex_;
    std::condition_variable wake_;

    mutable std::mutex sampleMutex_;
    SystemSample sample_;
};
