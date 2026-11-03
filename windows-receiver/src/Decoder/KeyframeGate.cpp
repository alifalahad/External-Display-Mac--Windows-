// =============================================================================
// KeyframeGate.cpp — Decode only frames that can be decoded correctly
// =============================================================================

#include "Decoder/KeyframeGate.h"

void KeyframeGate::reset() {
    haveKeyframe_ = false;
    damaged_ = false;
    haveNumber_ = false;
    requested_ = false;   // ask at once
}

bool KeyframeGate::admit(uint32_t frameNumber, bool numbered, bool keyframe, int64_t nowUs) {
    if (numbered && haveNumber_) {
        const int32_t step = static_cast<int32_t>(frameNumber - lastNumber_);
        if (step <= 0 && step > -10000) {
            held_++;   // late or duplicate: the picture is already past it
            return false;
        }
        if (step > 1 && step < 10000 && haveKeyframe_ && !damaged_ && !keyframe) {
            damaged_ = true;   // the frames in between are missing
            damagedSinceUs_ = nowUs;
            requested_ = false;
        }
        // A jump of 10000 or more: the Mac restarted its numbering
    }
    if (numbered) {
        haveNumber_ = true;
        lastNumber_ = frameNumber;
    }

    if (keyframe) {
        haveKeyframe_ = true;
        damaged_ = false;
        return true;
    }
    if (!haveKeyframe_ || (damaged_ && nowUs - damagedSinceUs_ < kMaxHoldUs)) {
        held_++;   // the decoder cannot start without a keyframe / freeze briefly
        return false;
    }
    return true;
}

bool KeyframeGate::shouldRequestKeyframe(int64_t nowUs) {
    if (!waitingForKeyframe()) return false;
    if (requested_ && nowUs - lastRequestUs_ < kRequestRetryUs) return false;
    requested_ = true;
    lastRequestUs_ = nowUs;
    requests_++;
    return true;
}
