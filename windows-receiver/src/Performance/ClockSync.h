#pragma once
// =============================================================================
// ClockSync.h — Mac ↔ PC clock offset from PING/PONG round trips
// =============================================================================
// The receiver sends PING (header.timestamp = local µs). The Mac answers PONG
// with its own clock in header.timestamp and echoes our timestamp in the
// payload. One round trip gives:
//
//     rtt    = localRecv − localSend
//     offset = remote − (localSend + localRecv) / 2       (remote − local)
//
// The offset is only exact if both directions take equally long; its error is
// at most ±rtt/2. Like NTP's clock filter we keep the last few samples and use
// the one with the smallest RTT — it had the least queueing, so the smallest
// error bound. Reset whenever the Mac's clock may have changed (reconnect).
//
// Portable C++ (no Windows headers) so it can be unit-tested on any OS.
// =============================================================================

#include <cstdint>
#include <deque>
#include <mutex>

class ClockSync {
public:
    struct Estimate {
        bool    valid = false;
        int64_t offsetUs = 0;       // remote − local
        double  errorBoundMs = 0;   // ± half the RTT of the chosen sample
        double  latestRttMs = 0;
        double  smoothedRttMs = 0;  // EWMA (1/8), what QUALITY_REPORT carries
        int     samples = 0;
    };

    /// Thread-safe. Times are microseconds; remote is the Mac's HostClock.
    void addSample(int64_t localSendUs, uint64_t remoteUs, int64_t localRecvUs);

    Estimate estimate() const;
    void reset();

    /// Remote (Mac) timestamp → local receiver clock
    static int64_t toLocalUs(uint64_t remoteUs, const Estimate& e) {
        return static_cast<int64_t>(remoteUs) - e.offsetUs;
    }

    static constexpr size_t  kWindow = 8;              // ~8 s at 1 ping/s
    static constexpr int64_t kMaxPlausibleRttUs = 2'000'000;

private:
    struct Sample {
        int64_t offsetUs;
        int64_t rttUs;
    };

    mutable std::mutex mutex_;
    std::deque<Sample> samples_;
    double latestRttMs_ = 0;
    double smoothedRttMs_ = 0;
};
