#pragma once
// =============================================================================
// PipelineClock.h — Receiver timestamps in microseconds (QueryPerformanceCounter)
// =============================================================================
// QPC is the timebase of DXGI_FRAME_STATISTICS::SyncQPCTime, so scan-out
// times compare directly with every other receiver timestamp.
// =============================================================================

#include <Windows.h>
#include <cstdint>

namespace PipelineClock {

inline int64_t Frequency() {
    static const int64_t frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    return frequency;
}

/// QPC ticks → µs without overflowing (ticks × 10⁶ would overflow after days)
inline int64_t QpcToUs(int64_t ticks) {
    const int64_t f = Frequency();
    return (ticks / f) * 1'000'000 + (ticks % f) * 1'000'000 / f;
}

inline int64_t NowUs() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return QpcToUs(now.QuadPart);
}

} // namespace PipelineClock
