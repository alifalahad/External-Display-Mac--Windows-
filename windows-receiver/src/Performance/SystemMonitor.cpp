// =============================================================================
// SystemMonitor.cpp — Process CPU / memory / GPU sampling
// =============================================================================

#include "Performance/SystemMonitor.h"
#include "Performance/PipelineClock.h"

#include <Windows.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <vector>

#pragma comment(lib, "pdh.lib")
#pragma comment(lib, "psapi.lib")

namespace {

uint64_t FileTimeTo100ns(const FILETIME& ft) {
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

/// "\GPU Engine(*)\Utilization Percentage" for this process, per engine type
class GpuEngineCounters {
public:
    GpuEngineCounters() {
        if (PdhOpenQueryW(nullptr, 0, &query_) != ERROR_SUCCESS) {
            query_ = nullptr;
            return;
        }
        if (PdhAddEnglishCounterW(query_, L"\\GPU Engine(*)\\Utilization Percentage",
                                  0, &counter_) != ERROR_SUCCESS) {
            PdhCloseQuery(query_);
            query_ = nullptr;
            return;
        }
        PdhCollectQueryData(query_);   // rate counter: first collection is the baseline
        swprintf(pidPrefix_, sizeof(pidPrefix_) / sizeof(pidPrefix_[0]),
                 L"pid_%lu_", GetCurrentProcessId());
    }

    ~GpuEngineCounters() {
        if (query_) PdhCloseQuery(query_);
    }

    bool available() const { return query_ != nullptr; }

    bool sample(double& out3d, double& outDecode) {
        out3d = outDecode = 0;
        if (!query_ || PdhCollectQueryData(query_) != ERROR_SUCCESS) return false;

        // Instances come and go (engines start/stop), so the size can change
        // between the sizing call and the read: retry a few times.
        for (int attempt = 0; attempt < 3; attempt++) {
            DWORD bytes = 0, count = 0;
            PDH_STATUS status = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE,
                                                             &bytes, &count, nullptr);
            if (status == ERROR_SUCCESS) return true;      // no instances at all
            if (status != static_cast<PDH_STATUS>(PDH_MORE_DATA)) return false;

            buffer_.resize(bytes);
            auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer_.data());
            status = PdhGetFormattedCounterArrayW(counter_, PDH_FMT_DOUBLE, &bytes, &count, items);
            if (status == static_cast<PDH_STATUS>(PDH_MORE_DATA)) continue;
            if (status != ERROR_SUCCESS) return false;

            const size_t prefixLen = wcslen(pidPrefix_);
            for (DWORD i = 0; i < count; i++) {
                const auto& item = items[i];
                if (item.FmtValue.CStatus != PDH_CSTATUS_VALID_DATA &&
                    item.FmtValue.CStatus != PDH_CSTATUS_NEW_DATA) continue;
                if (!item.szName || wcsncmp(item.szName, pidPrefix_, prefixLen) != 0) continue;
                const wchar_t* engine = wcsstr(item.szName, L"engtype_");
                if (!engine) continue;
                engine += 8;
                if (wcscmp(engine, L"3D") == 0)               out3d += item.FmtValue.doubleValue;
                else if (wcscmp(engine, L"VideoDecode") == 0) outDecode += item.FmtValue.doubleValue;
            }
            return true;
        }
        return false;
    }

private:
    PDH_HQUERY   query_ = nullptr;
    PDH_HCOUNTER counter_ = nullptr;
    wchar_t      pidPrefix_[32] = {};
    std::vector<uint8_t> buffer_;
};

} // namespace

SystemMonitor::SystemMonitor() {
    thread_ = std::thread(&SystemMonitor::run, this);
}

SystemMonitor::~SystemMonitor() {
    {
        std::lock_guard<std::mutex> lock(wakeMutex_);
        running_.store(false);
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
}

SystemSample SystemMonitor::latest() const {
    std::lock_guard<std::mutex> lock(sampleMutex_);
    return sample_;
}

void SystemMonitor::run() {
    GpuEngineCounters gpu;
    if (!gpu.available()) {
        printf("[Perf] GPU utilization counters unavailable — GPU %% will not be shown\n");
    }
    const double cores = static_cast<double>((std::max)(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS), 1UL));

    uint64_t lastCpu100ns = 0;
    int64_t lastWallUs = 0;

    while (running_.load()) {
        SystemSample s;

        FILETIME created, exited, kernel, user;
        if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
            uint64_t cpu100ns = FileTimeTo100ns(kernel) + FileTimeTo100ns(user);
            int64_t wallUs = PipelineClock::NowUs();
            if (lastWallUs != 0 && wallUs > lastWallUs) {
                double cpuUs = static_cast<double>(cpu100ns - lastCpu100ns) / 10.0;
                s.cpuPercent = cpuUs / static_cast<double>(wallUs - lastWallUs) / cores * 100.0;
                s.valid = true;
            }
            lastCpu100ns = cpu100ns;
            lastWallUs = wallUs;
        }

        PROCESS_MEMORY_COUNTERS_EX mem{};
        if (GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem))) {
            s.workingSetMB = static_cast<double>(mem.WorkingSetSize) / (1024.0 * 1024.0);
            s.privateMB = static_cast<double>(mem.PrivateUsage) / (1024.0 * 1024.0);
        }

        s.gpuValid = gpu.sample(s.gpu3dPercent, s.gpuDecodePercent);

        if (s.valid) {
            std::lock_guard<std::mutex> lock(sampleMutex_);
            sample_ = s;
        }

        std::unique_lock<std::mutex> lock(wakeMutex_);
        wake_.wait_for(lock, std::chrono::seconds(1), [this] { return !running_.load(); });
    }
}
