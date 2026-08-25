#pragma once
// =============================================================================
// FrameStats.h — High-precision frame timing and statistics
// =============================================================================
// Uses QueryPerformanceCounter for sub-microsecond accuracy.
// Maintains a rolling window of frame times for stable FPS reporting.
// =============================================================================

#include <Windows.h>
#include <cstdint>
#include <string>
#include <algorithm>

class FrameStats {
public:
    explicit FrameStats(float targetFPS = 60.0f);

    // Call at the very start of each frame
    void BeginFrame();

    // Call after Present() returns
    void EndFrame();

    // Reset all counters
    void Reset();

    // ── Accessors ───────────────────────────────────────────────────────────
    float    GetFPS() const            { return currentFPS_; }
    float    GetAvgFrameTimeMs() const { return avgFrameTimeMs_; }
    float    GetMinFrameTimeMs() const { return minFrameTimeMs_; }
    float    GetMaxFrameTimeMs() const { return maxFrameTimeMs_; }
    uint64_t GetDroppedFrames() const  { return droppedFrames_; }
    uint64_t GetTotalFrames() const    { return totalFrames_; }
    float    GetUptimeSeconds() const;

    // Format a human-readable stats string for console output
    std::wstring FormatStats() const;

private:
    float CounterToMs(LONGLONG ticks) const;

    LARGE_INTEGER frequency_{};
    LARGE_INTEGER frameStart_{};
    LARGE_INTEGER firstFrame_{};

    float targetFPS_;
    float targetFrameTimeMs_;

    // Rolling window for stable FPS/frame-time calculation
    static constexpr int kWindowSize = 120; // 2 seconds at 60 FPS
    float frameTimes_[kWindowSize] = {};
    int   writeIndex_  = 0;
    int   sampleCount_ = 0;

    // Computed values (updated every EndFrame)
    float currentFPS_      = 0.0f;
    float avgFrameTimeMs_  = 0.0f;
    float minFrameTimeMs_  = 999.0f;
    float maxFrameTimeMs_  = 0.0f;

    uint64_t totalFrames_  = 0;
    uint64_t droppedFrames_ = 0;
};
