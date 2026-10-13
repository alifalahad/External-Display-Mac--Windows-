#pragma once
// =============================================================================
// PipelineMetrics.h — Per-frame latency breakdown, rates and drops (Phase 16)
// =============================================================================
// Every frame carries a FrameTimeline from capture on the Mac to scan-out on
// this PC. Mac timestamps (HostClock µs) arrive in the frame's timing prefix;
// receiver timestamps are QPC µs. ClockSync maps one clock onto the other.
//
//   Capture  displayTime on the Mac        → handed to the encoder      (Mac)
//   Encode   handed to VideoToolbox        → encoder output             (Mac)
//   Send     encoder output                → packetization starts       (Mac)
//   Network  packetization starts          → last fragment received     (clock-synced)
//   Queue    frame complete                → decode starts              (PC)
//   Decode   decode starts                 → NV12 output                (PC)
//   Render   NV12 output                   → Present() returned         (PC: copy, upload, draw, present)
//   Display  Present() returned            → scan-out (vblank)          (PC: DXGI frame statistics)
//   Total    displayTime on the Mac        → scan-out (or Present)      (clock-synced)
//
// Portable C++ (no Windows headers) so the arithmetic is unit-tested on any OS.
// Not thread-safe: PipelineMetrics and BenchmarkRecorder live on the UI thread.
// =============================================================================

#include "Performance/ClockSync.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

// ── Per-frame timeline ──────────────────────────────────────────────────────

struct FrameTimeline {
    // Sender side (Mac HostClock µs), valid when hasSenderTiming
    bool     hasSenderTiming = false;
    bool     captureTimeValid = false;   // false for keyframe re-encodes of old content
    uint32_t frameSequence = 0;
    uint64_t captureUs = 0;
    uint64_t encodeStartUs = 0;
    uint64_t encodeEndUs = 0;
    uint64_t sendUs = 0;

    // Receiver side (QPC µs); 0 = not reached
    int64_t firstPacketUs = 0;
    int64_t completeUs = 0;
    int64_t decodeStartUs = 0;
    int64_t decodeEndUs = 0;
    int64_t presentUs = 0;
    int64_t displayUs = 0;

    uint32_t bytes = 0;
    bool     keyframe = false;
};

enum class Stage : int {
    Capture, Encode, Send, Network, Queue, Decode, Render, Display, Total, Count
};
constexpr int kStageCount = static_cast<int>(Stage::Count);
const char* StageName(Stage s);

using StageTimes = std::array<double, kStageCount>;   // ms; negative = unknown

/// Stage durations of one displayed frame. Stages needing the Mac↔PC clock
/// offset (Network, Total) are unknown until ClockSync has an estimate.
StageTimes ComputeStageTimes(const FrameTimeline& t, const ClockSync::Estimate& sync);

// ── Statistics helpers ──────────────────────────────────────────────────────

struct StageSummary {
    double   avgMs = 0;
    double   p50Ms = 0;
    double   p95Ms = 0;
    double   p99Ms = 0;
    double   maxMs = 0;
    uint64_t samples = 0;
};

/// Last N samples (live overlay)
class RollingSamples {
public:
    RollingSamples() : values_(120) {}   // ~2 s at 60 FPS
    explicit RollingSamples(size_t capacity) : values_(capacity) {}
    void add(double v);
    void clear() { count_ = 0; next_ = 0; }
    StageSummary summary() const;
private:
    std::vector<double> values_;
    size_t next_ = 0;
    size_t count_ = 0;
};

/// Fixed-memory histogram, 0.1 ms bins up to 1 s (benchmarks of any length:
/// memory must not grow during multi-hour stress runs)
class LatencyHistogram {
public:
    LatencyHistogram() : bins_(kBins, 0) {}
    void add(double ms);
    StageSummary summary() const;
private:
    static constexpr int    kBins = 10000;
    static constexpr double kBinMs = 0.1;
    std::vector<uint32_t> bins_;
    uint64_t count_ = 0;
    double   sum_ = 0;
    double   max_ = 0;
};

/// RFC 3550 interarrival jitter, from the Mac capture time and the PC arrival
/// time of each frame (clock offset cancels out — no sync needed)
class JitterEstimator {
public:
    void addFrame(uint64_t senderUs, int64_t arrivalUs);
    double jitterMs() const { return jitterUs_ / 1000.0; }
    void reset() { have_ = false; jitterUs_ = 0; }
private:
    bool     have_ = false;
    uint64_t lastSenderUs_ = 0;
    int64_t  lastArrivalUs_ = 0;
    double   jitterUs_ = 0;
};

// ── Live metrics ────────────────────────────────────────────────────────────

/// Network counters (StreamReceiver::Stats) the metrics turn into rates
struct NetworkCounters {
    uint64_t bytesReceived = 0;
    uint64_t packetsReceived = 0;
    uint64_t packetsLost = 0;
    uint64_t framesExpected = 0;     // from Mac frame sequence numbers
    uint64_t framesReceived = 0;     // complete frames
    double   jitterMs = 0;
};

struct MetricsSnapshot {
    std::array<StageSummary, kStageCount> stages{};
    double receivedFps = 0;
    double decodedFps = 0;
    double presentedFps = 0;         // new video frames on screen per second
    double loopFps = 0;              // render loop iterations (≈ refresh rate)
    double bitrateMbps = 0;
    double packetLossPercent = 0;
    double frameDropPercent = 0;     // expected but never shown, last interval
    double jitterMs = 0;
    uint64_t framesLostInNetwork = 0;   // since stream start
    uint64_t framesDroppedHere = 0;     // received but replaced/skipped before display
};

class PipelineMetrics {
public:
    void onLoopIteration() { loops_++; }
    void onFrameDecoded()  { decoded_++; }
    /// Received frames that will never reach the screen (queue trim, overwritten)
    void onFramesSkipped(uint64_t count) { skipped_ += count; }
    /// The frame's timeline is complete (shown, display time known or given up)
    StageTimes onFrameDisplayed(const FrameTimeline& t, const ClockSync::Estimate& sync);

    /// Turn counters into per-second rates. Call about once per second.
    void tick(int64_t nowUs, const NetworkCounters& net);

    MetricsSnapshot snapshot() const;

private:
    std::array<RollingSamples, kStageCount> live_{};

    uint64_t loops_ = 0, decoded_ = 0, presented_ = 0, skipped_ = 0;

    // Previous tick
    bool            haveTick_ = false;
    int64_t         lastTickUs_ = 0;
    NetworkCounters lastNet_{};
    uint64_t        lastLoops_ = 0, lastDecoded_ = 0, lastPresented_ = 0;
    uint64_t        firstExpected_ = 0, firstReceived_ = 0;
    bool            haveFirst_ = false;

    MetricsSnapshot rates_{};
};

// ── Benchmark recording (--bench=SECONDS) ───────────────────────────────────

/// Sender-side numbers from SENDER_STATS (Mac → PC, once per second)
struct SenderSample {
    bool   valid = false;
    double captureFps = 0;
    double encodeFps = 0;
    double cpuPercent = 0;           // of all Mac cores
    double memoryMB = 0;
    double encoderMbps = 0;
    double targetMbps = 0;
    bool   hardwareEncoder = false;
};

/// This process on the PC (SystemMonitor, once per second)
struct SystemSample {
    bool   valid = false;
    double cpuPercent = 0;           // of all PC cores
    double workingSetMB = 0;
    double privateMB = 0;
    bool   gpuValid = false;
    double gpu3dPercent = 0;
    double gpuDecodePercent = 0;
};

/// Writes <base>-frames.csv (one row per displayed frame), <base>-seconds.csv
/// (rates, CPU, GPU, RAM per second — shows leaks and slow degradation) and
/// <base>-summary.txt (percentiles over the whole run).
class BenchmarkRecorder {
public:
    bool start(const std::string& basePath, int durationSeconds, int64_t nowUs);
    bool active() const { return active_; }
    bool due(int64_t nowUs) const { return active_ && nowUs - startUs_ >= durationUs_; }

    void recordFrame(const FrameTimeline& t, const StageTimes& stages);
    void recordSecond(int64_t nowUs, const MetricsSnapshot& m, const SystemSample& sys,
                      const SenderSample& mac, const ClockSync::Estimate& sync);

    /// Close the files, write the summary; returns the summary text
    std::string finish(int64_t nowUs);
    const std::string& basePath() const { return base_; }

private:
    bool active_ = false;
    std::string base_;
    int64_t startUs_ = 0;
    int64_t durationUs_ = 0;
    std::ofstream frames_;
    std::ofstream seconds_;

    std::array<LatencyHistogram, kStageCount> histograms_{};
    uint64_t frameRows_ = 0;

    // Per-second aggregates for the summary
    struct Agg {
        double sum = 0, max = 0, first = 0, last = 0;
        uint64_t n = 0;
        void add(double v);
        double avg() const { return n ? sum / n : 0; }
    };
    Agg presentedFps_, receivedFps_, loopFps_, bitrate_, loss_, drop_, jitter_, rtt_;
    Agg pcCpu_, pcRam_, gpu3d_, gpuDecode_, macCpu_, macRam_, macCaptureFps_, macEncodeFps_;
    double clockErrorMs_ = 0;
    uint64_t lostFrames_ = 0, droppedFrames_ = 0;
};

// ── Overlay text ────────────────────────────────────────────────────────────

struct OverlayInputs {
    MetricsSnapshot metrics;
    ClockSync::Estimate sync;
    SenderSample mac;
    SystemSample pc;
    std::wstring connection;         // e.g. "Streaming 1920x1080"
    std::wstring gpuName;
    double loopFrameMsAvg = 0;
    double loopFrameMsMax = 0;
    bool   benchmarking = false;
};

std::wstring FormatDiagnostics(const OverlayInputs& in);
