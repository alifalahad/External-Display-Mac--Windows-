#pragma once
// =============================================================================
// DelayMonitor.h — Is a queue building up on the way? (portable, unit-tested)
// =============================================================================
// docs/SMOOTHNESS.md, step 6. Before a Wi-Fi link loses packets, its queues
// fill: frames take longer from the Mac's send to their first packet here.
//
//   one-way delay  = a frame's first packet to arrive (PC clock) − the Mac's
//                    send time in its header (Mac clock)
//   queuing delay  = one-way delay − its minimum over the last 10 s
//
// Packets, not complete frames: on an overloaded link most frames lose a
// packet, and the signal must not go blind exactly then.
//
// The two clocks differ by a constant (and drift by a few µs per second), so
// only the change matters, and the 10 s minimum cancels both. Every 100 ms the
// receiver reports the median queuing delay of the last 100 ms (spikes don't
// count), what arrived in the last 500 ms, and a verdict (RATE_FEEDBACK):
//   overuse  median above 15 ms and not falling → the Mac lowers the bitrate
//   normal   median below 5 ms                  → the Mac may raise it
//   hold     in between, or a queue that is draining after a cut
//
// What arrived shows what the link can carry only while it was full. So the
// report also says what arrived in the last 100 ms if every frame in that time
// waited at least 5 ms (the queue never emptied), else 0. A keyframe makes a
// queue that drains again within the window: the link had room, and the Mac
// does not cut for it. Ignoring keyframes' delays instead was tried: under
// congestion keyframes are requested again and again, and the detector went
// blind exactly then.
// =============================================================================

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>

enum class RateState : uint8_t { Normal = 0, Hold = 1, Overuse = 2 };

struct RateReport {
    uint32_t queuingDelayUs = 0;      // median, last 100 ms
    uint32_t receivedKbps = 0;        // everything that arrived (video, parity), last 500 ms
    uint16_t lossPercent100 = 0;      // packets, last 500 ms
    RateState state = RateState::Normal;
    uint8_t samples = 0;              // frames behind the median (0: no information)
    uint32_t linkKbps = 0;            // last 100 ms if the link was full the whole time, else 0
};

class DelayMonitor {
public:
    static constexpr int64_t  kReportIntervalUs = 100'000;
    static constexpr int64_t  kMedianWindowUs = 100'000;
    static constexpr int64_t  kRateWindowUs = 500'000;
    static constexpr int      kBaseSeconds = 10;
    static constexpr uint32_t kOveruseUs = 15'000;
    static constexpr uint32_t kNormalUs = 5'000;
    static constexpr int64_t  kFullLinkUs = 5'000;     // every frame waited at least this long

    /// A video packet arrived. macSendUs: the header's timestamp (the same for
    /// all packets of a frame); a new one starts a frame.
    void onPacket(uint64_t macSendUs, int64_t rxUs, size_t bytes);

    /// Every 100 ms a report is due (false before). packets*: totals so far.
    bool report(int64_t nowUs, uint64_t packetsReceived, uint64_t packetsLost, RateReport& out);

    /// New stream
    void reset();

private:
    struct Sample {                   // one per frame: its first packet
        int64_t arrivalUs;
        int64_t delayUs;              // one-way, including the clock offset
    };
    struct Bytes {                    // one per packet
        int64_t arrivalUs;
        uint32_t bytes;
    };
    struct Bucket {                   // smallest delay of one second
        int64_t second = INT64_MIN;
        int64_t minDelayUs = 0;
    };
    struct PacketMark {
        int64_t atUs;
        uint64_t received, lost;
    };

    std::deque<Sample> samples_;      // last 100 ms
    std::deque<Bytes> bytes_;         // last 500 ms
    bool haveFrame_ = false;
    uint64_t lastFrameSendUs_ = 0;
    std::array<Bucket, kBaseSeconds> buckets_{};
    std::deque<PacketMark> marks_;    // at each report, last 500 ms
    int64_t nextReportUs_ = 0;
    bool havePrevious_ = false;
    uint32_t previousMedianUs_ = 0;
};
