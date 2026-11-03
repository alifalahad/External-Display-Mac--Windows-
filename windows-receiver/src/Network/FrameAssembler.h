#pragma once
// =============================================================================
// FrameAssembler.h — Fragments → frames, rebuilding lost ones (portable, tested)
// =============================================================================
// docs/SMOOTHNESS.md, step 5. The Mac cuts each frame into fragments of
// 1,340 bytes and, when the receiver asks for it (RECEIVER_CAPS parity),
// adds parity packets: parity j of P is the XOR of fragments j, j+P, j+2P…,
// each zero-padded to the longest. A group that lost exactly one fragment is
// rebuilt here, so a lost packet — or a burst of up to P consecutive ones —
// costs no keyframe and no freeze.
//
// Frames are kept briefly after they complete, so a late fragment or parity
// packet does not start a new entry that can never complete.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

struct AssembledFrame {
    uint32_t frameSequence = 0;
    std::vector<uint8_t> data;
    bool keyframe = false;
    bool hasTiming = false;
    int64_t firstPacketUs = 0;
    uint16_t rebuilt = 0;            // fragments recovered from parity
};

class FrameAssembler {
public:
    static constexpr size_t   kFragmentBytes = 1340;   // exdp::MAX_FRAGMENT_PAYLOAD
    static constexpr uint16_t kMaxFragments = 8192;    // ~11 MB frame
    static constexpr uint32_t kMaxAge = 10;            // frames behind the newest: given up

    /// The packet's frame flags and arrival time
    struct PacketInfo {
        bool keyframe = false;
        bool hasTiming = false;
        int64_t rxUs = 0;
    };

    /// True when this packet completed the frame; `out` then holds it
    bool addFragment(uint32_t frame, uint16_t index, uint16_t total, const uint8_t* data, size_t len,
                     const PacketInfo& info, AssembledFrame& out);
    bool addParity(uint32_t frame, uint16_t total, uint16_t group, uint16_t groups, uint32_t frameBytes,
                   const uint8_t* data, size_t len, const PacketInfo& info, AssembledFrame& out);

    /// Forget frames more than kMaxAge behind the newest. Returns how many of
    /// them never completed: lost for good.
    size_t dropOld();
    /// New stream: the Mac numbers its frames from 0 again
    void reset();

    size_t incomplete() const;                // frames still waiting for fragments
    uint64_t fragmentsRebuilt() const { return fragmentsRebuilt_; }
    uint64_t framesRebuilt() const { return framesRebuilt_; }
    uint64_t parityPackets() const { return parityPackets_; }

private:
    struct Pending {
        uint16_t total = 0;
        uint16_t received = 0;
        std::vector<std::vector<uint8_t>> fragments;   // by index
        std::vector<bool> have;
        uint16_t groups = 0;                           // 0 until a parity packet arrived
        uint32_t frameBytes = 0;
        std::map<uint16_t, std::vector<uint8_t>> parity;
        bool keyframe = false;
        bool hasTiming = false;
        int64_t firstPacketUs = 0;
        uint16_t rebuilt = 0;
        bool done = false;                             // delivered; only remembered
    };

    bool admit(uint32_t frame);
    Pending* entry(uint32_t frame, uint16_t total, const PacketInfo& info);
    void repair(Pending& p);
    bool complete(uint32_t frame, Pending& p, AssembledFrame& out);

    std::map<uint32_t, Pending> frames_;
    bool haveNewest_ = false;
    uint32_t newest_ = 0;
    uint64_t fragmentsRebuilt_ = 0, framesRebuilt_ = 0, parityPackets_ = 0;
};
