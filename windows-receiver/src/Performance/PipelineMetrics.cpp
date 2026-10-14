// =============================================================================
// PipelineMetrics.cpp — Latency breakdown, rates, benchmark files, overlay text
// =============================================================================

#include "Performance/PipelineMetrics.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <sstream>

// ── Stage arithmetic ────────────────────────────────────────────────────────

const char* StageName(Stage s) {
    switch (s) {
        case Stage::Capture: return "Capture";
        case Stage::Encode:  return "Encode";
        case Stage::Send:    return "Send";
        case Stage::Network: return "Network";
        case Stage::Queue:   return "Queue";
        case Stage::Decode:  return "Decode";
        case Stage::Render:  return "Render";
        case Stage::Display: return "Display";
        case Stage::Total:   return "Total";
        default:             return "?";
    }
}

namespace {

/// b − a in ms when both timestamps exist and are ordered, else −1 (unknown)
double SpanMs(int64_t a, int64_t b) {
    if (a == 0 || b == 0 || b < a) return -1;
    return (b - a) / 1000.0;
}

double SpanMs(uint64_t a, uint64_t b) {
    if (a == 0 || b == 0 || b < a) return -1;
    return static_cast<double>(b - a) / 1000.0;
}

/// A span across the two clocks. It can come out slightly negative because the
/// offset is only known to ±errorBound; within that it is clamped to 0, beyond
/// it the sample is rejected as a sync failure.
double CrossClockSpanMs(int64_t fromLocalUs, int64_t toLocalUs, const ClockSync::Estimate& sync) {
    if (fromLocalUs == 0 || toLocalUs == 0) return -1;
    double ms = (toLocalUs - fromLocalUs) / 1000.0;
    if (ms >= 0) return ms;
    return (ms >= -(sync.errorBoundMs + 1.0)) ? 0.0 : -1;
}

size_t RankIndex(size_t n, double p) {
    size_t idx = static_cast<size_t>(std::ceil(p * static_cast<double>(n)));
    return idx == 0 ? 0 : (std::min)(idx - 1, n - 1);
}

} // namespace

StageTimes ComputeStageTimes(const FrameTimeline& t, const ClockSync::Estimate& sync) {
    StageTimes s;
    s.fill(-1);
    auto set = [&s](Stage st, double v) { s[static_cast<int>(st)] = v; };

    if (t.hasSenderTiming) {
        if (t.captureTimeValid) set(Stage::Capture, SpanMs(t.captureUs, t.encodeStartUs));
        set(Stage::Encode, SpanMs(t.encodeStartUs, t.encodeEndUs));
        set(Stage::Send, SpanMs(t.encodeEndUs, t.sendUs));

        if (sync.valid) {
            if (t.sendUs != 0) {
                set(Stage::Network,
                    CrossClockSpanMs(ClockSync::toLocalUs(t.sendUs, sync), t.completeUs, sync));
            }
            int64_t endUs = t.displayUs != 0 ? t.displayUs : t.presentUs;
            if (t.captureTimeValid && t.captureUs != 0) {
                set(Stage::Total,
                    CrossClockSpanMs(ClockSync::toLocalUs(t.captureUs, sync), endUs, sync));
            }
        }
    }

    set(Stage::Queue,   SpanMs(t.completeUs, t.decodeStartUs));
    set(Stage::Decode,  SpanMs(t.decodeStartUs, t.decodeEndUs));
    set(Stage::Render,  SpanMs(t.decodeEndUs, t.presentUs));
    set(Stage::Display, SpanMs(t.presentUs, t.displayUs));
    return s;
}

// ── RollingSamples ──────────────────────────────────────────────────────────

void RollingSamples::add(double v) {
    if (values_.empty()) return;
    values_[next_] = v;
    next_ = (next_ + 1) % values_.size();
    count_ = (std::min)(count_ + 1, values_.size());
}

StageSummary RollingSamples::summary() const {
    StageSummary out;
    if (count_ == 0) return out;
    std::vector<double> sorted(values_.begin(), values_.begin() + static_cast<std::ptrdiff_t>(count_));
    std::sort(sorted.begin(), sorted.end());
    double sum = 0;
    for (double v : sorted) sum += v;
    out.samples = count_;
    out.avgMs = sum / static_cast<double>(count_);
    out.p50Ms = sorted[RankIndex(count_, 0.50)];
    out.p95Ms = sorted[RankIndex(count_, 0.95)];
    out.p99Ms = sorted[RankIndex(count_, 0.99)];
    out.maxMs = sorted.back();
    return out;
}

// ── LatencyHistogram ────────────────────────────────────────────────────────

void LatencyHistogram::add(double ms) {
    if (ms < 0) return;
    int bin = static_cast<int>(ms / kBinMs);
    bins_[static_cast<size_t>((std::min)(bin, kBins - 1))]++;
    count_++;
    sum_ += ms;
    max_ = (std::max)(max_, ms);
}

StageSummary LatencyHistogram::summary() const {
    StageSummary out;
    if (count_ == 0) return out;
    out.samples = count_;
    out.avgMs = sum_ / static_cast<double>(count_);
    out.maxMs = max_;

    const double targets[3] = { 0.50, 0.95, 0.99 };
    double* outputs[3] = { &out.p50Ms, &out.p95Ms, &out.p99Ms };
    uint64_t cumulative = 0;
    int next = 0;
    for (int bin = 0; bin < kBins && next < 3; bin++) {
        cumulative += bins_[static_cast<size_t>(bin)];
        while (next < 3 && cumulative >= RankIndex(count_, targets[next]) + 1) {
            // Bin midpoint, never above the true maximum (last bin is open-ended)
            *outputs[next] = (std::min)((bin + 0.5) * kBinMs, max_);
            next++;
        }
    }
    return out;
}

// ── JitterEstimator ─────────────────────────────────────────────────────────

void JitterEstimator::addFrame(uint64_t senderUs, int64_t arrivalUs) {
    if (senderUs == 0 || arrivalUs == 0) return;
    if (have_ && senderUs > lastSenderUs_) {
        double d = static_cast<double>(arrivalUs - lastArrivalUs_)
                 - static_cast<double>(senderUs - lastSenderUs_);
        jitterUs_ += (std::fabs(d) - jitterUs_) / 16.0;
    }
    if (!have_ || senderUs > lastSenderUs_) {
        have_ = true;
        lastSenderUs_ = senderUs;
        lastArrivalUs_ = arrivalUs;
    }
}

// ── PipelineMetrics ─────────────────────────────────────────────────────────

StageTimes PipelineMetrics::onFrameDisplayed(const FrameTimeline& t, const ClockSync::Estimate& sync) {
    StageTimes stages = ComputeStageTimes(t, sync);
    for (int i = 0; i < kStageCount; i++) {
        if (stages[static_cast<size_t>(i)] >= 0) live_[static_cast<size_t>(i)].add(stages[static_cast<size_t>(i)]);
    }
    presented_++;
    return stages;
}

void PipelineMetrics::tick(int64_t nowUs, const NetworkCounters& net) {
    // Counters going backwards = receiver restarted them; take a new baseline
    bool restarted = net.framesReceived < lastNet_.framesReceived ||
                     net.bytesReceived < lastNet_.bytesReceived;
    if (!haveFirst_ || restarted) {
        haveFirst_ = true;
        firstExpected_ = net.framesExpected;
        firstReceived_ = net.framesReceived;
    }

    if (haveTick_ && !restarted && nowUs > lastTickUs_) {
        double dt = (nowUs - lastTickUs_) / 1e6;
        auto rate = [dt](uint64_t now, uint64_t before) {
            return now >= before ? static_cast<double>(now - before) / dt : 0.0;
        };
        rates_.receivedFps  = rate(net.framesReceived, lastNet_.framesReceived);
        rates_.decodedFps   = rate(decoded_, lastDecoded_);
        rates_.presentedFps = rate(presented_, lastPresented_);
        rates_.renderedFps  = rate(renders_, lastRenders_);
        rates_.bitrateMbps  = rate(net.bytesReceived, lastNet_.bytesReceived) * 8.0 / 1e6;

        uint64_t got  = net.packetsReceived - lastNet_.packetsReceived;
        uint64_t lost = net.packetsLost >= lastNet_.packetsLost ? net.packetsLost - lastNet_.packetsLost : 0;
        rates_.packetLossPercent = (got + lost) ? 100.0 * static_cast<double>(lost) / static_cast<double>(got + lost) : 0;

        uint64_t expected = net.framesExpected >= lastNet_.framesExpected
                          ? net.framesExpected - lastNet_.framesExpected : 0;
        uint64_t shown = presented_ - lastPresented_;
        rates_.frameDropPercent = expected > shown
            ? 100.0 * static_cast<double>(expected - shown) / static_cast<double>(expected) : 0;
    }

    rates_.jitterMs = net.jitterMs;
    uint64_t expectedTotal = net.framesExpected - firstExpected_;
    uint64_t receivedTotal = net.framesReceived - firstReceived_;
    rates_.framesLostInNetwork = expectedTotal > receivedTotal ? expectedTotal - receivedTotal : 0;
    rates_.framesDroppedHere = skipped_;

    haveTick_ = true;
    lastTickUs_ = nowUs;
    lastNet_ = net;
    lastRenders_ = renders_;
    lastDecoded_ = decoded_;
    lastPresented_ = presented_;
}

MetricsSnapshot PipelineMetrics::snapshot() const {
    MetricsSnapshot out = rates_;
    out.renderCall = renderCall_.summary();
    for (int i = 0; i < kStageCount; i++) out.stages[static_cast<size_t>(i)] = live_[static_cast<size_t>(i)].summary();
    return out;
}

// ── BenchmarkRecorder ───────────────────────────────────────────────────────

void BenchmarkRecorder::Agg::add(double v) {
    if (n == 0) first = v;
    last = v;
    sum += v;
    max = n == 0 ? v : (std::max)(max, v);
    n++;
}

bool BenchmarkRecorder::start(const std::string& basePath, int durationSeconds, int64_t nowUs) {
    base_ = basePath;
    frames_.open(base_ + "-frames.csv");
    seconds_.open(base_ + "-seconds.csv");
    if (!frames_ || !seconds_) {
        frames_.close();
        seconds_.close();
        return false;
    }
    frames_ << "frame_seq,keyframe,bytes";
    for (int i = 0; i < kStageCount; i++) frames_ << "," << StageName(static_cast<Stage>(i)) << "_ms";
    frames_ << "\n";
    seconds_ << "t_s,received_fps,decoded_fps,presented_fps,rendered_fps,bitrate_mbps,"
                "packet_loss_pct,frame_drop_pct,jitter_ms,rtt_ms,clock_error_ms,"
                "pc_cpu_pct,pc_ram_mb,pc_private_mb,gpu_3d_pct,gpu_decode_pct,"
                "mac_capture_fps,mac_encode_fps,mac_cpu_pct,mac_ram_mb,mac_encoder_mbps,mac_target_mbps,"
                "total_avg_ms,total_p95_ms\n";
    startUs_ = nowUs;
    durationUs_ = static_cast<int64_t>(durationSeconds) * 1'000'000;
    active_ = true;
    return true;
}

void BenchmarkRecorder::recordFrame(const FrameTimeline& t, const StageTimes& stages) {
    if (!active_) return;
    frames_ << t.frameSequence << ',' << (t.keyframe ? 1 : 0) << ',' << t.bytes;
    char buf[32];
    for (int i = 0; i < kStageCount; i++) {
        double v = stages[static_cast<size_t>(i)];
        frames_ << ',';
        if (v >= 0) {
            snprintf(buf, sizeof(buf), "%.3f", v);
            frames_ << buf;
            histograms_[static_cast<size_t>(i)].add(v);
        }
    }
    frames_ << '\n';
    frameRows_++;
}

void BenchmarkRecorder::recordSecond(int64_t nowUs, const MetricsSnapshot& m, const SystemSample& sys,
                                     const SenderSample& mac, const ClockSync::Estimate& sync) {
    if (!active_) return;
    auto opt = [](bool valid, double v) {
        if (!valid) return std::string();
        char b[32];
        snprintf(b, sizeof(b), "%.2f", v);
        return std::string(b);
    };
    const StageSummary& total = m.stages[static_cast<size_t>(Stage::Total)];
    seconds_ << opt(true, (nowUs - startUs_) / 1e6) << ','
             << opt(true, m.receivedFps) << ',' << opt(true, m.decodedFps) << ','
             << opt(true, m.presentedFps) << ',' << opt(true, m.renderedFps) << ','
             << opt(true, m.bitrateMbps) << ',' << opt(true, m.packetLossPercent) << ','
             << opt(true, m.frameDropPercent) << ',' << opt(true, m.jitterMs) << ','
             << opt(sync.valid, sync.smoothedRttMs) << ',' << opt(sync.valid, sync.errorBoundMs) << ','
             << opt(sys.valid, sys.cpuPercent) << ',' << opt(sys.valid, sys.workingSetMB) << ','
             << opt(sys.valid, sys.privateMB) << ','
             << opt(sys.gpuValid, sys.gpu3dPercent) << ',' << opt(sys.gpuValid, sys.gpuDecodePercent) << ','
             << opt(mac.valid, mac.captureFps) << ',' << opt(mac.valid, mac.encodeFps) << ','
             << opt(mac.valid, mac.cpuPercent) << ',' << opt(mac.valid, mac.memoryMB) << ','
             << opt(mac.valid, mac.encoderMbps) << ',' << opt(mac.valid, mac.targetMbps) << ','
             << opt(total.samples > 0, total.avgMs) << ',' << opt(total.samples > 0, total.p95Ms) << '\n';

    presentedFps_.add(m.presentedFps);
    receivedFps_.add(m.receivedFps);
    renderedFps_.add(m.renderedFps);
    bitrate_.add(m.bitrateMbps);
    loss_.add(m.packetLossPercent);
    drop_.add(m.frameDropPercent);
    jitter_.add(m.jitterMs);
    if (sync.valid) {
        rtt_.add(sync.smoothedRttMs);
        clockErrorMs_ = sync.errorBoundMs;
    }
    if (sys.valid) { pcCpu_.add(sys.cpuPercent); pcRam_.add(sys.workingSetMB); }
    if (sys.gpuValid) { gpu3d_.add(sys.gpu3dPercent); gpuDecode_.add(sys.gpuDecodePercent); }
    if (mac.valid) {
        macCpu_.add(mac.cpuPercent);
        macRam_.add(mac.memoryMB);
        macCaptureFps_.add(mac.captureFps);
        macEncodeFps_.add(mac.encodeFps);
    }
    lostFrames_ = m.framesLostInNetwork;
    droppedFrames_ = m.framesDroppedHere;
}

std::string BenchmarkRecorder::finish(int64_t nowUs) {
    if (!active_) return {};
    active_ = false;
    frames_.close();
    seconds_.close();

    std::ostringstream o;
    char line[256];
    o << "External Display - benchmark summary\n";
    snprintf(line, sizeof(line), "Duration: %.1f s   Frames displayed: %llu\n\n",
             (nowUs - startUs_) / 1e6, static_cast<unsigned long long>(frameRows_));
    o << line;
    for (const auto& note : notes_) o << note << "\n";
    if (!notes_.empty()) o << "\n";
    o << "Stage (ms)      avg     p50     p95     p99     max   samples\n";
    for (int i = 0; i < kStageCount; i++) {
        StageSummary s = histograms_[static_cast<size_t>(i)].summary();
        if (s.samples == 0) {
            snprintf(line, sizeof(line), "%-10s   (not measured)\n", StageName(static_cast<Stage>(i)));
        } else {
            snprintf(line, sizeof(line), "%-10s %7.2f %7.2f %7.2f %7.2f %7.2f   %llu\n",
                     StageName(static_cast<Stage>(i)), s.avgMs, s.p50Ms, s.p95Ms, s.p99Ms, s.maxMs,
                     static_cast<unsigned long long>(s.samples));
        }
        o << line;
    }
    snprintf(line, sizeof(line), "(Network and Total use the Mac-PC clock offset, accurate to +/-%.2f ms)\n\n",
             clockErrorMs_);
    o << line;

    snprintf(line, sizeof(line), "FPS        presented %.1f avg   received %.1f   rendered %.1f\n",
             presentedFps_.avg(), receivedFps_.avg(), renderedFps_.avg());
    o << line;
    snprintf(line, sizeof(line), "Network    %.1f Mbps avg (%.1f max)   RTT %.1f ms   jitter %.2f ms   packet loss %.3f%%\n",
             bitrate_.avg(), bitrate_.max, rtt_.avg(), jitter_.avg(), loss_.avg());
    o << line;
    snprintf(line, sizeof(line), "Drops      %.3f%% of frames avg   lost in network %llu   dropped on PC %llu\n",
             drop_.avg(), static_cast<unsigned long long>(lostFrames_),
             static_cast<unsigned long long>(droppedFrames_));
    o << line;
    if (pcCpu_.n) {
        snprintf(line, sizeof(line), "PC         CPU %.1f%% avg (%.1f%% max, all cores)   RAM %.0f -> %.0f MB (peak %.0f)\n",
                 pcCpu_.avg(), pcCpu_.max, pcRam_.first, pcRam_.last, pcRam_.max);
        o << line;
    }
    if (gpu3d_.n) {
        snprintf(line, sizeof(line), "PC GPU     3D %.1f%% avg (%.1f%% max)   video decode %.1f%% avg (%.1f%% max)\n",
                 gpu3d_.avg(), gpu3d_.max, gpuDecode_.avg(), gpuDecode_.max);
        o << line;
    } else {
        o << "PC GPU     not available (GPU performance counters missing)\n";
    }
    if (macCpu_.n) {
        snprintf(line, sizeof(line), "Mac        capture %.1f FPS   encode %.1f FPS   CPU %.1f%% avg (%.1f%% max, all cores)   RAM %.0f -> %.0f MB\n",
                 macCaptureFps_.avg(), macEncodeFps_.avg(), macCpu_.avg(), macCpu_.max,
                 macRam_.first, macRam_.last);
        o << line;
    } else {
        o << "Mac        no SENDER_STATS received (old Mac build?)\n";
    }

    std::string text = o.str();
    std::ofstream summary(base_ + "-summary.txt");
    summary << text;
    return text;
}

// ── Overlay text ────────────────────────────────────────────────────────────

namespace {

void Appendf(std::wstring& out, const wchar_t* fmt, ...) {
    wchar_t buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vswprintf(buf, sizeof(buf) / sizeof(buf[0]), fmt, args);
    va_end(args);
    if (n > 0) out.append(buf, static_cast<size_t>((std::min)(n, 255)));
}

void AppendStage(std::wstring& out, const wchar_t* name, const StageSummary& s, const wchar_t* note) {
    if (s.samples == 0) {
        Appendf(out, L"  %-8ls     n/a                  %ls\n", name, note);
    } else {
        Appendf(out, L"  %-8ls %6.1f %6.1f %6.1f   %ls\n", name, s.avgMs, s.p95Ms, s.maxMs, note);
    }
}

} // namespace

std::wstring FormatDiagnostics(const OverlayInputs& in) {
    const MetricsSnapshot& m = in.metrics;
    auto stage = [&m](Stage s) -> const StageSummary& { return m.stages[static_cast<size_t>(s)]; };
    std::wstring out;
    out.reserve(1400);

    Appendf(out, L"%ls%ls\n", in.connection.c_str(), in.benchmarking ? L"   [BENCHMARK]" : L"");

    if (in.mac.valid) {
        Appendf(out, L"FPS      capture %.1f  encode %.1f  recv %.1f\n",
                in.mac.captureFps, in.mac.encodeFps, m.receivedFps);
    } else {
        Appendf(out, L"FPS      recv %.1f\n", m.receivedFps);
    }
    Appendf(out, L"         decode %.1f (%ls)  shown %.1f\n", m.decodedFps,
            in.hardwareDecode ? L"GPU" : L"CPU", m.presentedFps);
    Appendf(out, L"Render   %.1f/s, %.2f ms CPU avg, %.2f max\n", m.renderedFps,
            m.renderCall.avgMs, m.renderCall.maxMs);

    if (in.sync.valid) {
        Appendf(out, L"Network  %.1f Mbps  RTT %.1f ms  jitter %.1f ms\n",
                m.bitrateMbps, in.sync.smoothedRttMs, m.jitterMs);
    } else {
        Appendf(out, L"Network  %.1f Mbps  RTT ...  jitter %.1f ms\n", m.bitrateMbps, m.jitterMs);
    }
    Appendf(out, L"Dropped  %.2f%%  (net lost %llu, PC skipped %llu)\n",
            m.frameDropPercent, static_cast<unsigned long long>(m.framesLostInNetwork),
            static_cast<unsigned long long>(m.framesDroppedHere));
    Appendf(out, L"Loss     %.2f%% packets\n", m.packetLossPercent);

    out += L"Latency ms    avg    p95    max\n";
    AppendStage(out, L"Capture", stage(Stage::Capture), L"Mac");
    AppendStage(out, L"Encode",  stage(Stage::Encode),  L"Mac");
    AppendStage(out, L"Send",    stage(Stage::Send),    L"Mac");
    AppendStage(out, L"Network", stage(Stage::Network), in.sync.valid ? L"" : L"syncing clocks");
    AppendStage(out, L"Queue",   stage(Stage::Queue),   L"");
    AppendStage(out, L"Decode",  stage(Stage::Decode),  L"");
    AppendStage(out, L"Render",  stage(Stage::Render),  L"");
    AppendStage(out, L"Display", stage(Stage::Display), L"to scan-out");
    AppendStage(out, L"Total",   stage(Stage::Total),
                stage(Stage::Display).samples ? L"capture to screen" : L"capture to Present");
    if (in.sync.valid) Appendf(out, L"  clock sync +/-%.1f ms\n", in.sync.errorBoundMs);

    if (in.mac.valid) {
        Appendf(out, L"Mac  CPU %.1f%%  RAM %.0f MB  enc %.1f/%.0f Mbps %ls\n",
                in.mac.cpuPercent, in.mac.memoryMB, in.mac.encoderMbps, in.mac.targetMbps,
                in.mac.hardwareEncoder ? L"HW" : L"SW");
    }
    if (in.pc.valid) {
        Appendf(out, L"PC   CPU %.1f%%  RAM %.0f MB", in.pc.cpuPercent, in.pc.workingSetMB);
        if (in.pc.gpuValid) {
            Appendf(out, L"  GPU 3D %.0f%% dec %.0f%%", in.pc.gpu3dPercent, in.pc.gpuDecodePercent);
        }
        out += L"\n";
    }
    Appendf(out, L"GPU  %ls", in.gpuName.c_str());
    return out;
}
