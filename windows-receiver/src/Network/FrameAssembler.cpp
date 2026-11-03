// =============================================================================
// FrameAssembler.cpp — Fragments → frames, rebuilding lost ones
// =============================================================================

#include "Network/FrameAssembler.h"

bool FrameAssembler::admit(uint32_t frame) {
    if (!haveNewest_) {
        haveNewest_ = true;
        newest_ = frame;
        return true;
    }
    const int32_t ahead = static_cast<int32_t>(frame - newest_);
    if (ahead > 0) {
        newest_ = frame;
        return true;
    }
    if (ahead >= -static_cast<int32_t>(kMaxAge)) return true;   // still being assembled
    if (ahead < -10000) {                                       // the Mac restarted its numbering
        newest_ = frame;
        return true;
    }
    return false;                                               // given up already
}

FrameAssembler::Pending* FrameAssembler::entry(uint32_t frame, uint16_t total, const PacketInfo& info) {
    auto it = frames_.find(frame);
    if (it == frames_.end()) {
        Pending p;
        p.total = total;
        p.fragments.resize(total);
        p.have.assign(total, false);
        p.keyframe = info.keyframe;
        p.hasTiming = info.hasTiming;
        p.firstPacketUs = info.rxUs;
        it = frames_.emplace(frame, std::move(p)).first;
    } else if (it->second.total != total) {
        return nullptr;   // inconsistent with earlier packets
    }
    return &it->second;
}

bool FrameAssembler::addFragment(uint32_t frame, uint16_t index, uint16_t total, const uint8_t* data,
                                 size_t len, const PacketInfo& info, AssembledFrame& out) {
    if (total == 0 || total > kMaxFragments || index >= total || len > kFragmentBytes) return false;
    if (!admit(frame)) return false;
    Pending* p = entry(frame, total, info);
    if (!p || p->done || p->have[index]) return false;   // inconsistent, complete already, duplicate
    p->fragments[index].assign(data, data + len);
    p->have[index] = true;
    p->received++;
    repair(*p);
    return complete(frame, *p, out);
}

bool FrameAssembler::addParity(uint32_t frame, uint16_t total, uint16_t group, uint16_t groups,
                               uint32_t frameBytes, const uint8_t* data, size_t len,
                               const PacketInfo& info, AssembledFrame& out) {
    if (total == 0 || total > kMaxFragments || groups == 0 || groups > total || group >= groups ||
        len == 0 || len > kFragmentBytes) {
        return false;
    }
    // The frame's size must fit its fragment count (it gives the last fragment's length)
    const uint64_t fullBefore = static_cast<uint64_t>(total - 1) * kFragmentBytes;
    if (frameBytes <= fullBefore || frameBytes > fullBefore + kFragmentBytes) return false;
    if (!admit(frame)) return false;
    parityPackets_++;
    Pending* p = entry(frame, total, info);
    if (!p || p->done) return false;
    if (p->groups && (p->groups != groups || p->frameBytes != frameBytes)) return false;
    p->groups = groups;
    p->frameBytes = frameBytes;
    if (!p->parity.emplace(group, std::vector<uint8_t>(data, data + len)).second) return false;   // duplicate
    repair(*p);
    return complete(frame, *p, out);
}

void FrameAssembler::repair(Pending& p) {
    if (p.groups == 0 || p.received == p.total) return;
    for (auto& [group, xorData] : p.parity) {
        int missing = -1, missingCount = 0;
        for (uint32_t i = group; i < p.total; i += p.groups) {
            if (!p.have[i]) {
                missing = static_cast<int>(i);
                if (++missingCount > 1) break;
            }
        }
        if (missingCount != 1) continue;   // complete, or two lost: needs a keyframe

        const uint32_t m = static_cast<uint32_t>(missing);
        const size_t length = m + 1 < p.total ? kFragmentBytes
                            : p.frameBytes - static_cast<size_t>(p.total - 1) * kFragmentBytes;
        if (xorData.size() < length) continue;   // malformed
        std::vector<uint8_t> rebuilt(xorData);
        bool consistent = true;
        for (uint32_t i = group; i < p.total; i += p.groups) {
            if (i == m) continue;
            const std::vector<uint8_t>& f = p.fragments[i];
            // Every fragment but the last is full size, or the lengths don't add up
            if (f.size() > rebuilt.size() || (i + 1 < p.total && f.size() != kFragmentBytes)) {
                consistent = false;
                break;
            }
            for (size_t k = 0; k < f.size(); k++) rebuilt[k] ^= f[k];
        }
        if (!consistent) continue;
        rebuilt.resize(length);
        p.fragments[m] = std::move(rebuilt);
        p.have[m] = true;
        p.received++;
        p.rebuilt++;
        fragmentsRebuilt_++;
    }
}

bool FrameAssembler::complete(uint32_t frame, Pending& p, AssembledFrame& out) {
    if (p.received != p.total) return false;
    out.frameSequence = frame;
    out.keyframe = p.keyframe;
    out.hasTiming = p.hasTiming;
    out.firstPacketUs = p.firstPacketUs;
    out.rebuilt = p.rebuilt;
    size_t size = 0;
    for (const auto& f : p.fragments) size += f.size();
    out.data.clear();
    out.data.reserve(size);
    for (const auto& f : p.fragments) out.data.insert(out.data.end(), f.begin(), f.end());
    if (p.rebuilt) framesRebuilt_++;

    // Remember only that it is done: late packets for it are ignored
    p.done = true;
    p.fragments.clear();
    p.fragments.shrink_to_fit();
    p.have.clear();
    p.parity.clear();
    return true;
}

size_t FrameAssembler::dropOld() {
    size_t lost = 0;
    for (auto it = frames_.begin(); it != frames_.end();) {
        const int32_t behind = static_cast<int32_t>(newest_ - it->first);
        if (behind > static_cast<int32_t>(kMaxAge) || behind < -10000) {
            if (!it->second.done && behind > 0) lost++;
            it = frames_.erase(it);
        } else {
            ++it;
        }
    }
    return lost;
}

void FrameAssembler::reset() {
    frames_.clear();
    haveNewest_ = false;
}

size_t FrameAssembler::incomplete() const {
    size_t n = 0;
    for (const auto& [frame, p] : frames_) n += p.done ? 0 : 1;
    return n;
}
