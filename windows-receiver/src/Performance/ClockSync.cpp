// =============================================================================
// ClockSync.cpp — min-RTT clock offset filter
// =============================================================================

#include "Performance/ClockSync.h"

void ClockSync::addSample(int64_t localSendUs, uint64_t remoteUs, int64_t localRecvUs) {
    int64_t rttUs = localRecvUs - localSendUs;
    if (rttUs < 0 || rttUs > kMaxPlausibleRttUs || remoteUs == 0) return;  // stale or bogus

    // remote − midpoint; written to avoid overflow of (send + recv)
    int64_t midpointUs = localSendUs + rttUs / 2;
    Sample sample{ static_cast<int64_t>(remoteUs) - midpointUs, rttUs };

    std::lock_guard<std::mutex> lock(mutex_);
    samples_.push_back(sample);
    if (samples_.size() > kWindow) samples_.pop_front();

    latestRttMs_ = rttUs / 1000.0;
    smoothedRttMs_ = (smoothedRttMs_ == 0)
        ? latestRttMs_
        : smoothedRttMs_ + (latestRttMs_ - smoothedRttMs_) / 8.0;
}

ClockSync::Estimate ClockSync::estimate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Estimate e;
    if (samples_.empty()) return e;

    const Sample* best = &samples_.front();
    for (const auto& s : samples_) {
        if (s.rttUs < best->rttUs) best = &s;
    }
    e.valid = true;
    e.offsetUs = best->offsetUs;
    e.errorBoundMs = best->rttUs / 2000.0;
    e.latestRttMs = latestRttMs_;
    e.smoothedRttMs = smoothedRttMs_;
    e.samples = static_cast<int>(samples_.size());
    return e;
}

void ClockSync::reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    samples_.clear();
    latestRttMs_ = 0;
    smoothedRttMs_ = 0;
}
