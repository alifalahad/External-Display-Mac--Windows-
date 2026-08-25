// =============================================================================
// FrameStats.cpp — High-precision frame timing implementation
// =============================================================================

#include "Performance/FrameStats.h"
#include <cstdio>
#include <cmath>

FrameStats::FrameStats(float targetFPS)
    : targetFPS_(targetFPS)
    , targetFrameTimeMs_(1000.0f / targetFPS)
{
    QueryPerformanceFrequency(&frequency_);
    QueryPerformanceCounter(&firstFrame_);
    frameStart_ = firstFrame_;
}

void FrameStats::BeginFrame() {
    QueryPerformanceCounter(&frameStart_);
}

void FrameStats::EndFrame() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);

    float frameTimeMs = CounterToMs(now.QuadPart - frameStart_.QuadPart);

    // Store in rolling window
    frameTimes_[writeIndex_] = frameTimeMs;
    writeIndex_ = (writeIndex_ + 1) % kWindowSize;
    if (sampleCount_ < kWindowSize) {
        sampleCount_++;
    }

    totalFrames_++;

    // A "dropped frame" is one that took more than 1.5x the target frame time
    // (e.g., >25ms at 60 FPS target = 16.67ms)
    if (frameTimeMs > targetFrameTimeMs_ * 1.5f) {
        droppedFrames_++;
    }

    // Compute statistics from the rolling window
    if (sampleCount_ > 0) {
        float sum = 0.0f;
        float minVal = 999999.0f;
        float maxVal = 0.0f;

        for (int i = 0; i < sampleCount_; i++) {
            float t = frameTimes_[i];
            sum += t;
            minVal = (std::min)(minVal, t);
            maxVal = (std::max)(maxVal, t);
        }

        avgFrameTimeMs_ = sum / static_cast<float>(sampleCount_);
        minFrameTimeMs_ = minVal;
        maxFrameTimeMs_ = maxVal;

        if (avgFrameTimeMs_ > 0.0f) {
            currentFPS_ = 1000.0f / avgFrameTimeMs_;
        }
    }
}

void FrameStats::Reset() {
    writeIndex_ = 0;
    sampleCount_ = 0;
    totalFrames_ = 0;
    droppedFrames_ = 0;
    currentFPS_ = 0.0f;
    avgFrameTimeMs_ = 0.0f;
    minFrameTimeMs_ = 999.0f;
    maxFrameTimeMs_ = 0.0f;
    QueryPerformanceCounter(&firstFrame_);
}

float FrameStats::GetUptimeSeconds() const {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return static_cast<float>(now.QuadPart - firstFrame_.QuadPart)
         / static_cast<float>(frequency_.QuadPart);
}

float FrameStats::CounterToMs(LONGLONG ticks) const {
    return static_cast<float>(ticks) * 1000.0f
         / static_cast<float>(frequency_.QuadPart);
}

std::wstring FrameStats::FormatStats() const {
    wchar_t buf[512];
    float uptime = GetUptimeSeconds();
    float dropPct = (totalFrames_ > 0)
        ? (static_cast<float>(droppedFrames_) / static_cast<float>(totalFrames_) * 100.0f)
        : 0.0f;

    swprintf_s(buf, sizeof(buf) / sizeof(buf[0]),
        L"FPS: %.1f  |  Frame: %.2f ms avg  %.2f min  %.2f max  |  "
        L"Dropped: %llu/%llu (%.2f%%)  |  Uptime: %.1fs",
        currentFPS_,
        avgFrameTimeMs_, minFrameTimeMs_, maxFrameTimeMs_,
        droppedFrames_, totalFrames_, dropPct,
        uptime);

    return std::wstring(buf);
}
