// =============================================================================
// PipelineMetricsTests.cpp — Unit tests for the portable Phase 16 code
// =============================================================================
// ClockSync, stage arithmetic, percentiles, jitter, rates, benchmark files and
// the wire layout of the Phase 16 payloads. No Windows headers: builds with
// MSVC (cmake -DEXDP_BUILD_TESTS=ON) and with clang/gcc on any OS:
//
//   c++ -std=c++17 -Isrc tests/PipelineMetricsTests.cpp \
//       src/Performance/ClockSync.cpp src/Performance/PipelineMetrics.cpp
// =============================================================================

#include "Performance/ClockSync.h"
#include "Performance/PipelineMetrics.h"
#include "Network/Protocol.h"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <string>

static int gFailures = 0;
static int gChecks = 0;

#define CHECK(cond) do { gChecks++; if (!(cond)) { gFailures++; \
    std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_NEAR(a, b, tol) do { gChecks++; double _a = (a), _b = (b); \
    if (std::fabs(_a - _b) > (tol)) { gFailures++; \
    std::printf("  FAIL %s:%d  %s = %.4f, expected %.4f (+/-%.4f)\n", \
                __FILE__, __LINE__, #a, _a, _b, (double)(tol)); } } while (0)

static double StageOf(const StageTimes& s, Stage st) { return s[static_cast<size_t>(st)]; }

// ── ClockSync ───────────────────────────────────────────────────────────────

static void TestClockSyncPicksMinRttSample() {
    std::printf("ClockSync: min-RTT sample wins, offset within error bound\n");
    const int64_t trueOffset = 7'000'000'000LL;   // Mac clock is 7000 s ahead
    ClockSync sync;
    std::mt19937 rng(42);
    std::uniform_int_distribution<int> queueing(0, 20000);   // up to 20 ms extra, one way
    int64_t local = 1'000'000;
    for (int i = 0; i < 8; i++) {
        int64_t out = 1000 + queueing(rng);        // PC → Mac
        int64_t back = 1000 + queueing(rng);       // Mac → PC (asymmetric)
        uint64_t remote = static_cast<uint64_t>(local + out + trueOffset);
        sync.addSample(local, remote, local + out + back);
        local += 1'000'000;
    }
    ClockSync::Estimate e = sync.estimate();
    CHECK(e.valid);
    CHECK(e.samples == 8);
    // The guarantee: |error| ≤ rtt/2 of the chosen sample
    CHECK(std::fabs((e.offsetUs - trueOffset) / 1000.0) <= e.errorBoundMs + 1e-9);

    // A perfectly symmetric fast sample must give the exact offset
    sync.addSample(local, static_cast<uint64_t>(local + 500 + trueOffset), local + 1000);
    e = sync.estimate();
    CHECK(e.offsetUs == trueOffset);
    CHECK_NEAR(e.errorBoundMs, 0.5, 1e-9);
    CHECK_NEAR(e.latestRttMs, 1.0, 1e-9);
}

static void TestClockSyncRejectsAndResets() {
    std::printf("ClockSync: bogus samples rejected, window bounded, reset\n");
    ClockSync sync;
    sync.addSample(1000, 5000, 900);          // negative RTT
    sync.addSample(1000, 5000, 1000 + 3'000'000);  // 3 s RTT
    sync.addSample(1000, 0, 2000);            // no remote time
    CHECK(!sync.estimate().valid);

    for (int i = 0; i < 20; i++) sync.addSample(i * 1000, 100000, i * 1000 + 200);
    CHECK(sync.estimate().samples == static_cast<int>(ClockSync::kWindow));
    sync.reset();
    CHECK(!sync.estimate().valid);
}

// ── Stage arithmetic ────────────────────────────────────────────────────────

static FrameTimeline MakeTimeline(int64_t offsetUs) {
    // Mac clock = PC clock + offset. Times in PC µs, converted for Mac fields.
    FrameTimeline t;
    t.hasSenderTiming = true;
    t.captureTimeValid = true;
    t.captureUs     = static_cast<uint64_t>(100'000 + offsetUs);
    t.encodeStartUs = static_cast<uint64_t>(101'800 + offsetUs);   // capture 1.8
    t.encodeEndUs   = static_cast<uint64_t>(103'900 + offsetUs);   // encode 2.1
    t.sendUs        = static_cast<uint64_t>(104'200 + offsetUs);   // send 0.3
    t.firstPacketUs = 105'000;
    t.completeUs    = 107'200;   // network 3.0
    t.decodeStartUs = 108'000;   // queue 0.8
    t.decodeEndUs   = 109'400;   // decode 1.4
    t.presentUs     = 111'000;   // render 1.6
    t.displayUs     = 118'000;   // display 7.0
    return t;                    // total 18.0
}

static void TestStageTimes() {
    std::printf("ComputeStageTimes: every stage from a synthetic timeline\n");
    const int64_t offset = -3'500'000;   // Mac clock behind the PC
    ClockSync::Estimate sync;
    sync.valid = true;
    sync.offsetUs = offset;
    sync.errorBoundMs = 0.5;

    StageTimes s = ComputeStageTimes(MakeTimeline(offset), sync);
    CHECK_NEAR(StageOf(s, Stage::Capture), 1.8, 1e-9);
    CHECK_NEAR(StageOf(s, Stage::Encode),  2.1, 1e-9);
    CHECK_NEAR(StageOf(s, Stage::Send),    0.3, 1e-9);
    CHECK_NEAR(StageOf(s, Stage::Network), 3.0, 1e-9);
    CHECK_NEAR(StageOf(s, Stage::Queue),   0.8, 1e-9);
    CHECK_NEAR(StageOf(s, Stage::Decode),  1.4, 1e-9);
    CHECK_NEAR(StageOf(s, Stage::Render),  1.6, 1e-9);
    CHECK_NEAR(StageOf(s, Stage::Display), 7.0, 1e-9);
    CHECK_NEAR(StageOf(s, Stage::Total),  18.0, 1e-9);

    // Stages add up to the total
    double sum = 0;
    for (int i = 0; i < static_cast<int>(Stage::Total); i++) sum += s[static_cast<size_t>(i)];
    CHECK_NEAR(sum, StageOf(s, Stage::Total), 1e-9);
}

static void TestStageTimesUnknowns() {
    std::printf("ComputeStageTimes: missing data stays unknown, never invented\n");
    ClockSync::Estimate noSync;   // not yet synchronized
    FrameTimeline t = MakeTimeline(0);
    StageTimes s = ComputeStageTimes(t, noSync);
    CHECK(StageOf(s, Stage::Network) < 0);
    CHECK(StageOf(s, Stage::Total) < 0);
    CHECK(StageOf(s, Stage::Encode) > 0);    // single-clock stages still known

    // Re-encoded keyframe: capture time is synthetic → no capture/total
    ClockSync::Estimate sync;
    sync.valid = true;
    t.captureTimeValid = false;
    s = ComputeStageTimes(t, sync);
    CHECK(StageOf(s, Stage::Capture) < 0);
    CHECK(StageOf(s, Stage::Total) < 0);
    CHECK(StageOf(s, Stage::Network) >= 0);

    // No DXGI frame statistics: Display unknown, Total measured to Present
    t = MakeTimeline(0);
    t.displayUs = 0;
    s = ComputeStageTimes(t, sync);
    CHECK(StageOf(s, Stage::Display) < 0);
    CHECK_NEAR(StageOf(s, Stage::Total), 11.0, 1e-9);

    // Old Mac build: no timing prefix at all
    FrameTimeline bare;
    bare.completeUs = 1000; bare.decodeStartUs = 1500; bare.decodeEndUs = 2500; bare.presentUs = 4000;
    s = ComputeStageTimes(bare, sync);
    CHECK(StageOf(s, Stage::Encode) < 0);
    CHECK_NEAR(StageOf(s, Stage::Decode), 1.0, 1e-9);
}

static void TestCrossClockClamp() {
    std::printf("ComputeStageTimes: sync error clamps within bound, rejects beyond\n");
    ClockSync::Estimate sync;
    sync.valid = true;
    sync.errorBoundMs = 2.0;
    FrameTimeline t = MakeTimeline(0);
    t.completeUs = static_cast<int64_t>(t.sendUs) - 1500;     // −1.5 ms: inside ±(2+1)
    CHECK_NEAR(StageOf(ComputeStageTimes(t, sync), Stage::Network), 0.0, 1e-9);
    t.completeUs = static_cast<int64_t>(t.sendUs) - 10000;    // −10 ms: impossible
    CHECK(StageOf(ComputeStageTimes(t, sync), Stage::Network) < 0);
}

// ── Statistics ──────────────────────────────────────────────────────────────

static void TestPercentiles() {
    std::printf("RollingSamples / LatencyHistogram: percentiles\n");
    RollingSamples rolling(1000);
    LatencyHistogram hist;
    for (int i = 1; i <= 1000; i++) {   // 0.1 .. 100.0 ms
        rolling.add(i / 10.0);
        hist.add(i / 10.0);
    }
    StageSummary r = rolling.summary();
    CHECK(r.samples == 1000);
    CHECK_NEAR(r.avgMs, 50.05, 1e-9);
    CHECK_NEAR(r.p50Ms, 50.0, 1e-9);
    CHECK_NEAR(r.p95Ms, 95.0, 1e-9);
    CHECK_NEAR(r.p99Ms, 99.0, 1e-9);
    CHECK_NEAR(r.maxMs, 100.0, 1e-9);

    StageSummary h = hist.summary();
    CHECK(h.samples == 1000);
    CHECK_NEAR(h.avgMs, 50.05, 1e-6);
    CHECK_NEAR(h.p50Ms, 50.0, 0.1);   // histogram resolution
    CHECK_NEAR(h.p95Ms, 95.0, 0.1);
    CHECK_NEAR(h.p99Ms, 99.0, 0.1);
    CHECK_NEAR(h.maxMs, 100.0, 1e-9);

    // Window keeps only the newest samples
    RollingSamples small(4);
    for (double v : { 100.0, 100.0, 1.0, 2.0, 3.0, 4.0 }) small.add(v);
    CHECK_NEAR(small.summary().maxMs, 4.0, 1e-9);

    // Outliers beyond the histogram range land in the last bin, max stays exact
    LatencyHistogram big;
    big.add(5000.0);
    CHECK_NEAR(big.summary().maxMs, 5000.0, 1e-9);
    CHECK(big.summary().p99Ms <= 5000.0);
}

static void TestJitter() {
    std::printf("JitterEstimator: RFC 3550 behaviour\n");
    JitterEstimator steady;
    for (int i = 0; i < 200; i++) {
        steady.addFrame(1'000'000 + i * 16667ULL, 5'000'000 + i * 16667LL);   // constant delay
    }
    CHECK_NEAR(steady.jitterMs(), 0.0, 1e-9);

    JitterEstimator wobbly;
    for (int i = 0; i < 400; i++) {
        int64_t delay = (i % 2 == 0) ? 2000 : 0;   // alternating 0 / 2 ms delay
        wobbly.addFrame(1'000'000 + i * 16667ULL, 5'000'000 + i * 16667LL + delay);
    }
    CHECK_NEAR(wobbly.jitterMs(), 2.0, 0.01);       // |D| = 2 ms every frame
}

static void TestRates() {
    std::printf("PipelineMetrics: rates, drops and loss from counters\n");
    PipelineMetrics m;
    ClockSync::Estimate sync;
    NetworkCounters net;
    m.tick(0, net);   // baseline

    // One second: 60 frames expected, 58 received, 57 shown, 1 skipped
    FrameTimeline t = MakeTimeline(0);
    for (int i = 0; i < 57; i++) { m.onFrameDecoded(); m.onFrameDisplayed(t, sync); }
    m.onFrameDecoded();
    m.onFramesSkipped(1);
    for (int i = 0; i < 60; i++) m.onLoopIteration();
    net.framesExpected = 60;
    net.framesReceived = 58;
    net.bytesReceived = 5'000'000;     // 40 Mbit
    net.packetsReceived = 990;
    net.packetsLost = 10;
    net.jitterMs = 0.7;
    m.tick(1'000'000, net);

    MetricsSnapshot s = m.snapshot();
    CHECK_NEAR(s.receivedFps, 58, 1e-9);
    CHECK_NEAR(s.decodedFps, 58, 1e-9);
    CHECK_NEAR(s.presentedFps, 57, 1e-9);
    CHECK_NEAR(s.loopFps, 60, 1e-9);
    CHECK_NEAR(s.bitrateMbps, 40.0, 1e-9);
    CHECK_NEAR(s.packetLossPercent, 1.0, 1e-9);
    CHECK_NEAR(s.frameDropPercent, 5.0, 1e-9);     // 3 of 60 never shown
    CHECK(s.framesLostInNetwork == 2);
    CHECK(s.framesDroppedHere == 1);
    CHECK_NEAR(s.jitterMs, 0.7, 1e-9);
    CHECK(s.stages[static_cast<size_t>(Stage::Decode)].samples == 57);

    // Receiver counters restarted (reconnect): no negative or huge rates
    NetworkCounters restarted;
    restarted.framesReceived = 3;
    restarted.bytesReceived = 1000;
    m.tick(2'000'000, restarted);
    s = m.snapshot();
    CHECK(s.receivedFps >= 0 && s.receivedFps < 1000);
    CHECK(s.framesLostInNetwork == 0);
}

// ── Benchmark files ─────────────────────────────────────────────────────────

static int CountLines(const std::string& path) {
    std::ifstream f(path);
    int n = 0;
    std::string line;
    while (std::getline(f, line)) n++;
    return n;
}

static void TestBenchmarkRecorder() {
    std::printf("BenchmarkRecorder: CSV rows, summary percentiles\n");
    std::string base = "pipeline-test-bench";
    BenchmarkRecorder rec;
    CHECK(rec.start(base, 2, 0));
    CHECK(!rec.due(1'999'999));
    CHECK(rec.due(2'000'000));

    ClockSync::Estimate sync;
    sync.valid = true;
    sync.errorBoundMs = 0.4;
    for (int i = 0; i < 120; i++) {
        FrameTimeline t = MakeTimeline(0);
        t.frameSequence = static_cast<uint32_t>(i);
        rec.recordFrame(t, ComputeStageTimes(t, sync));
    }
    MetricsSnapshot m;
    m.presentedFps = 60;
    SystemSample sys;
    sys.valid = true;
    sys.cpuPercent = 3;
    sys.workingSetMB = 90;
    SenderSample mac;
    rec.recordSecond(1'000'000, m, sys, mac, sync);
    rec.recordSecond(2'000'000, m, sys, mac, sync);
    std::string summary = rec.finish(2'000'000);

    CHECK(CountLines(base + "-frames.csv") == 121);    // header + 120
    CHECK(CountLines(base + "-seconds.csv") == 3);     // header + 2
    CHECK(summary.find("Total") != std::string::npos);
    CHECK(summary.find("18.00") != std::string::npos); // total latency of every frame
    CHECK(summary.find("Frames displayed: 120") != std::string::npos);
    std::ifstream written(base + "-summary.txt");
    std::stringstream ss;
    ss << written.rdbuf();
    CHECK(ss.str() == summary);

    std::remove((base + "-frames.csv").c_str());
    std::remove((base + "-seconds.csv").c_str());
    std::remove((base + "-summary.txt").c_str());
}

static void TestOverlayText() {
    std::printf("FormatDiagnostics: produces every section\n");
    OverlayInputs in;
    in.connection = L"Streaming 1920x1080";
    in.gpuName = L"Test GPU";
    in.sync.valid = true;
    in.sync.smoothedRttMs = 3.2;
    in.mac.valid = true;
    in.mac.captureFps = 59.9;
    in.pc.valid = true;
    in.pc.gpuValid = true;
    in.metrics.stages[static_cast<size_t>(Stage::Total)].samples = 10;
    in.metrics.stages[static_cast<size_t>(Stage::Total)].avgMs = 27.5;
    std::wstring text = FormatDiagnostics(in);
    CHECK(text.find(L"capture 59.9") != std::wstring::npos);
    CHECK(text.find(L"RTT 3.2 ms") != std::wstring::npos);
    CHECK(text.find(L"27.5") != std::wstring::npos);
    CHECK(text.find(L"GPU 3D") != std::wstring::npos);
    CHECK(text.find(L"Test GPU") != std::wstring::npos);
}

// ── Wire layout (must match Protocol.swift) ─────────────────────────────────

static void TestWireLayout() {
    std::printf("Protocol: Phase 16 payload byte layout\n");
    // FrameTimingPrefix as serialized by Protocol.swift: LE u32,u32,u64×4
    uint8_t bytes[40] = {};
    auto putLE = [&bytes](int at, uint64_t v, int n) {
        for (int i = 0; i < n; i++) bytes[at + i] = static_cast<uint8_t>(v >> (8 * i));
    };
    putLE(0, 0x01020304, 4);
    putLE(4, exdp::TIMING_CAPTURE_VALID, 4);
    putLE(8, 0x1111111111ULL, 8);
    putLE(16, 0x2222222222ULL, 8);
    putLE(24, 0x3333333333ULL, 8);
    putLE(32, 0x4444444444ULL, 8);
    exdp::FrameTimingPrefix p;
    std::memcpy(&p, bytes, sizeof(p));
    CHECK(p.frameSequence == 0x01020304);
    CHECK(p.flags == exdp::TIMING_CAPTURE_VALID);
    CHECK(p.captureUs == 0x1111111111ULL);
    CHECK(p.encodeStartUs == 0x2222222222ULL);
    CHECK(p.encodeEndUs == 0x3333333333ULL);
    CHECK(p.sendUs == 0x4444444444ULL);

    CHECK(sizeof(exdp::SenderStatsPayload) == 32);
    CHECK(offsetof(exdp::SenderStatsPayload, flags) == 24);
    CHECK(offsetof(exdp::QualityReportPayload, latencyUs) == 16);
    CHECK(static_cast<uint8_t>(exdp::MessageType::SenderStats) == 0x51);
    CHECK(exdp::FLAG_TIMING == 2);
}

int main() {
    TestClockSyncPicksMinRttSample();
    TestClockSyncRejectsAndResets();
    TestStageTimes();
    TestStageTimesUnknowns();
    TestCrossClockClamp();
    TestPercentiles();
    TestJitter();
    TestRates();
    TestBenchmarkRecorder();
    TestOverlayText();
    TestWireLayout();

    std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
