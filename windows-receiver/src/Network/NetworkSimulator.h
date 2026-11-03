#pragma once
// =============================================================================
// NetworkSimulator.h — A worse network on purpose, for tests (portable, tested)
// =============================================================================
// Both act on incoming UDP packets before anything else sees them, so the
// whole receiver reacts as on a real network:
//   LossSimulator  (--simulate-loss)  drops packets: tests loss repair (step 5)
//   LinkSimulator  (--simulate-link)  a narrow link with a queue: packets are
//                  held until they leave it, a full queue drops them — tests
//                  the bitrate that follows the delay (step 6)
// =============================================================================

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

/// Drops packets on purpose to test loss repair (--simulate-loss): about
/// `percent` of them, in runs of `burst` consecutive packets
class LossSimulator {
public:
    void configure(double percent, int burst, uint64_t seed = 0x9E3779B97F4A7C15ULL) {
        burst_ = burst < 1 ? 1 : burst;
        eventProbability_ = percent > 0 ? percent / 100.0 / burst_ : 0;
        state_ = seed ? seed : 1;
        left_ = 0;
    }
    bool active() const { return eventProbability_ > 0; }
    /// Decide for the next packet
    bool drop() {
        if (!active()) return false;
        if (left_ > 0) { left_--; return true; }
        state_ ^= state_ << 13;                      // xorshift64
        state_ ^= state_ >> 7;
        state_ ^= state_ << 17;
        if (static_cast<double>(state_ >> 11) / 9007199254740992.0 >= eventProbability_) return false;
        left_ = burst_ - 1;
        return true;
    }

private:
    double eventProbability_ = 0;
    int burst_ = 1;
    int left_ = 0;
    uint64_t state_ = 1;
};

/// A link of `kbps` with a queue of `bufferMs`: a packet waits until the ones
/// before it have left; when the queue is full it is dropped (tail drop).
/// The receiver holds each packet until it leaves the link, so everything after
/// it (the delay feedback, frame assembly, keyframe requests) learns of it only
/// as late as it would behind a real queue.
class LinkSimulator {
public:
    void configure(double kbps, double bufferMs = 200) {
        bytesPerUs_ = kbps > 0 ? kbps * 1000.0 / 8.0 / 1e6 : 0;
        bufferUs_ = static_cast<int64_t>(bufferMs * 1000);
        reset();
    }
    bool active() const { return bytesPerUs_ > 0; }
    /// An empty link (a new connection)
    void reset() {
        busyUntilUs_ = 0;
        held_.clear();
    }
    /// A packet of `bytes` arrives at `nowUs`. False: dropped. Otherwise
    /// `exitUs` = when it leaves the link (its arrival time from now on).
    bool pass(int64_t nowUs, size_t bytes, int64_t& exitUs) {
        const int64_t start = std::max(nowUs, busyUntilUs_);
        if (start - nowUs > bufferUs_) return false;
        busyUntilUs_ = start + static_cast<int64_t>(static_cast<double>(bytes) / bytesPerUs_);
        exitUs = busyUntilUs_;
        return true;
    }

    /// pass(), keeping the packet until it leaves the link. False: dropped.
    bool hold(int64_t nowUs, const uint8_t* data, size_t len) {
        int64_t exitUs = 0;
        if (!pass(nowUs, len, exitUs)) return false;
        held_.push_back({ exitUs, std::vector<uint8_t>(data, data + len) });
        return true;
    }
    /// When the next held packet leaves the link (INT64_MAX: none held)
    int64_t nextExitUs() const { return held_.empty() ? INT64_MAX : held_.front().exitUs; }
    /// The oldest held packet if it has left the link by `nowUs`: its bytes,
    /// and its exit time (from then on its arrival time)
    bool release(int64_t nowUs, std::vector<uint8_t>& data, int64_t& exitUs) {
        if (held_.empty() || held_.front().exitUs > nowUs) return false;
        exitUs = held_.front().exitUs;
        data = std::move(held_.front().data);
        held_.pop_front();
        return true;
    }

private:
    struct Held {
        int64_t exitUs;
        std::vector<uint8_t> data;
    };
    double bytesPerUs_ = 0;
    int64_t bufferUs_ = 0;
    int64_t busyUntilUs_ = 0;
    std::deque<Held> held_;                        // in exit order: the link is first in, first out
};
