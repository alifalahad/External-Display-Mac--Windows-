#pragma once
// =============================================================================
// KeyframeGate.h — Decode only frames that can be decoded correctly (portable, tested)
// =============================================================================
// docs/SMOOTHNESS.md, step 2. The Mac makes keyframes only when asked: they
// are several times larger than other frames, and one per second made the
// Wi-Fi burst every second. So a missing frame (lost on the network,
// incomplete, or trimmed from the decode queue) must be noticed here, because
// the frames after it build on it. The Mac numbers its frames consecutively.
//
//   no keyframe since the decoder (re)started → hold every other frame
//   a frame number was skipped                → hold for up to 300 ms
//   either way: ask for a keyframe at once, then every 250 ms until one comes
//
// Holding keeps the last good picture on screen: a few frames of freeze look
// better than smeared blocks. If the keyframe is late, decoding goes on after
// 300 ms, damaged until the keyframe repairs it.
// =============================================================================

#include <cstdint>

class KeyframeGate {
public:
    static constexpr int64_t kRequestRetryUs = 250'000;
    static constexpr int64_t kMaxHoldUs = 300'000;

    /// New stream or new decoder: nothing decodes before a keyframe
    void reset();
    /// Decode this frame? `numbered`: it carries the Mac's frame number (an
    /// older Mac app's frames don't; they are not checked for gaps).
    bool admit(uint32_t frameNumber, bool numbered, bool keyframe, int64_t nowUs);
    /// Time to send KEYFRAME_REQ? Call on every loop; counts the requests.
    bool shouldRequestKeyframe(int64_t nowUs);

    bool waitingForKeyframe() const { return !haveKeyframe_ || damaged_; }
    uint64_t framesHeld() const { return held_; }
    uint64_t requests() const { return requests_; }

private:
    bool haveKeyframe_ = false;     // since the last reset
    bool damaged_ = false;          // a frame went missing after it
    int64_t damagedSinceUs_ = 0;
    bool haveNumber_ = false;
    uint32_t lastNumber_ = 0;
    bool requested_ = false;        // already asked during this wait
    int64_t lastRequestUs_ = 0;
    uint64_t held_ = 0;
    uint64_t requests_ = 0;
};
