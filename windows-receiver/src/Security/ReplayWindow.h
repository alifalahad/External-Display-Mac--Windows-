#pragma once
// =============================================================================
// ReplayWindow.h — Reject replayed or very old UDP packets (RFC 4303 §3.4.3)
// =============================================================================
// Counters may arrive out of order (Wi-Fi reordering) and with gaps (loss).
// A counter is accepted once, if it is within the last 64 of the highest seen.
// Call accept() only AFTER the packet's authentication tag verified, so a
// forged packet cannot advance the window.
// Portable C++ (no Windows headers).
// =============================================================================

#include <cstdint>

class ReplayWindow {
public:
    static constexpr uint64_t kSize = 64;

    /// Would this counter be accepted? (check before decrypting: cheap rejects)
    bool isFresh(uint64_t counter) const {
        if (!any_ || counter > highest_) return true;
        uint64_t behind = highest_ - counter;
        return behind < kSize && !(bitmap_ & (1ULL << behind));
    }

    /// Record an authenticated counter; false if it was a replay / too old
    bool accept(uint64_t counter) {
        if (!isFresh(counter)) return false;
        if (!any_) {
            any_ = true;
            highest_ = counter;
            bitmap_ = 1;
        } else if (counter > highest_) {
            uint64_t shift = counter - highest_;
            bitmap_ = shift >= kSize ? 1 : (bitmap_ << shift) | 1;
            highest_ = counter;
        } else {
            bitmap_ |= 1ULL << (highest_ - counter);
        }
        return true;
    }

    void reset() { any_ = false; highest_ = 0; bitmap_ = 0; }

private:
    bool     any_ = false;
    uint64_t highest_ = 0;
    uint64_t bitmap_ = 0;   // bit i = (highest_ - i) seen
};
