// =============================================================================
// DelayMonitor.cpp — Is a queue building up on the way?
// =============================================================================

#include "Network/DelayMonitor.h"

#include <algorithm>
#include <climits>
#include <vector>

namespace {
int64_t SecondOf(int64_t us) { return us >= 0 ? us / 1'000'000 : (us - 999'999) / 1'000'000; }
}

void DelayMonitor::onPacket(uint64_t macSendUs, int64_t rxUs, size_t bytes) {
    bytes_.push_back({ rxUs, static_cast<uint32_t>(bytes) });
    if (haveFrame_ && macSendUs == lastFrameSendUs_) return;   // the rest of a frame
    haveFrame_ = true;
    lastFrameSendUs_ = macSendUs;

    const int64_t delay = rxUs - static_cast<int64_t>(macSendUs);
    samples_.push_back({ rxUs, delay });
    const int64_t second = SecondOf(rxUs);
    Bucket& b = buckets_[static_cast<size_t>(((second % kBaseSeconds) + kBaseSeconds) % kBaseSeconds)];
    if (b.second != second) {
        b.second = second;
        b.minDelayUs = delay;
    } else {
        b.minDelayUs = std::min(b.minDelayUs, delay);
    }
}

bool DelayMonitor::report(int64_t nowUs, uint64_t packetsReceived, uint64_t packetsLost, RateReport& out) {
    if (nowUs < nextReportUs_) return false;
    nextReportUs_ = nowUs + kReportIntervalUs;

    while (!samples_.empty() && samples_.front().arrivalUs < nowUs - kMedianWindowUs) samples_.pop_front();
    while (!bytes_.empty() && bytes_.front().arrivalUs < nowUs - kRateWindowUs) bytes_.pop_front();

    // The smallest delay of the last 10 s: the delay without any queue
    const int64_t nowSecond = SecondOf(nowUs);
    bool haveBase = false;
    int64_t base = 0;
    for (const Bucket& b : buckets_) {
        // Arrival times can run ahead of the clock (a simulated link): count those too
        if (b.second == INT64_MIN || b.second <= nowSecond - kBaseSeconds) continue;
        base = haveBase ? std::min(base, b.minDelayUs) : b.minDelayUs;
        haveBase = true;
    }

    RateReport r;
    std::vector<int64_t> queuing;
    for (const Sample& s : samples_) {
        if (haveBase) queuing.push_back(s.delayUs - base);
    }
    uint64_t bytes = 0, recentBytes = 0;
    for (const Bytes& b : bytes_) {
        bytes += b.bytes;
        if (b.arrivalUs >= nowUs - kMedianWindowUs) recentBytes += b.bytes;
    }
    // Over the time really covered: at a stream start the window is not full yet
    const int64_t span = bytes_.empty() ? kRateWindowUs
                       : std::min(kRateWindowUs, std::max<int64_t>(kMedianWindowUs, nowUs - bytes_.front().arrivalUs));
    r.receivedKbps = static_cast<uint32_t>(bytes * 8 * 1000 / static_cast<uint64_t>(span));
    r.samples = static_cast<uint8_t>(std::min<size_t>(queuing.size(), 255));

    // Loss over the last 500 ms, from the totals at earlier reports
    marks_.push_back({ nowUs, packetsReceived, packetsLost });
    while (marks_.size() > 1 && marks_[1].atUs <= nowUs - kRateWindowUs) marks_.pop_front();
    const PacketMark& then = marks_.front();
    const uint64_t got = packetsReceived - then.received, lost = packetsLost - then.lost;
    r.lossPercent100 = got + lost ? static_cast<uint16_t>(std::min<uint64_t>(10000, lost * 10000 / (got + lost))) : 0;

    if (queuing.empty()) {
        r.state = RateState::Normal;   // nothing arrived: samples 0, the Mac ignores it
    } else {
        // No frame got through without waiting: the link was busy all the time,
        // so what arrived is what it can carry
        const int64_t shortest = *std::min_element(queuing.begin(), queuing.end());
        if (queuing.size() >= 2 && shortest >= kFullLinkUs) {
            r.linkKbps = static_cast<uint32_t>(recentBytes * 8 * 1000 / static_cast<uint64_t>(kMedianWindowUs));
        }
        std::nth_element(queuing.begin(), queuing.begin() + static_cast<std::ptrdiff_t>(queuing.size() / 2), queuing.end());
        const int64_t median = std::max<int64_t>(0, queuing[queuing.size() / 2]);
        r.queuingDelayUs = static_cast<uint32_t>(std::min<int64_t>(median, UINT32_MAX));
        const bool falling = havePrevious_ && r.queuingDelayUs + 1000 < previousMedianUs_;
        if (r.queuingDelayUs > kOveruseUs && !falling) r.state = RateState::Overuse;
        else if (r.queuingDelayUs < kNormalUs) r.state = RateState::Normal;
        else r.state = RateState::Hold;
        havePrevious_ = true;
        previousMedianUs_ = r.queuingDelayUs;
    }
    out = r;
    return true;
}

void DelayMonitor::reset() {
    samples_.clear();
    bytes_.clear();
    haveFrame_ = false;
    buckets_.fill(Bucket{});
    marks_.clear();
    nextReportUs_ = 0;
    havePrevious_ = false;
    previousMedianUs_ = 0;
}
