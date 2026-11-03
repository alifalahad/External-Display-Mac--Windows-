// =============================================================================
// NetworkTests.cpp — Loss repair and the delay feedback (docs/SMOOTHNESS.md, steps 5–6)
// =============================================================================
// Parity bytes identical to the Mac's (shared vectors, also computed with
// Python), the parity header layout, frame assembly with every kind of loss,
// the loss and link simulators, and the delay monitor behind RATE_FEEDBACK.
// Portable: builds with MSVC and with clang/gcc.
// =============================================================================

#include "Network/DelayMonitor.h"
#include "Network/FrameAssembler.h"
#include "Network/NetworkSimulator.h"
#include "Network/Protocol.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <set>
#include <string>
#include <vector>

static int gChecks = 0, gFailures = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFailures++; \
    std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

constexpr size_t kSize = FrameAssembler::kFragmentBytes;

static std::vector<uint8_t> Frame(size_t n) {
    std::vector<uint8_t> f(n);
    for (size_t i = 0; i < n; i++) f[i] = static_cast<uint8_t>(i * 7 + 3);
    return f;
}

static uint16_t FragmentCount(const std::vector<uint8_t>& f) { return static_cast<uint16_t>((f.size() + kSize - 1) / kSize); }

static std::vector<uint8_t> Fragment(const std::vector<uint8_t>& f, size_t i) {
    const size_t start = i * kSize, end = std::min(start + kSize, f.size());
    return std::vector<uint8_t>(f.begin() + static_cast<std::ptrdiff_t>(start), f.begin() + static_cast<std::ptrdiff_t>(end));
}

/// What the Mac's FrameParity.parity computes, written independently
static std::vector<uint8_t> Parity(const std::vector<uint8_t>& f, size_t group, size_t groups) {
    const size_t total = FragmentCount(f);
    size_t length = 0;
    for (size_t i = group; i < total; i += groups) length = std::max(length, Fragment(f, i).size());
    std::vector<uint8_t> out(length, 0);
    for (size_t i = group; i < total; i += groups) {
        std::vector<uint8_t> frag = Fragment(f, i);
        for (size_t k = 0; k < frag.size(); k++) out[k] ^= frag[k];
    }
    return out;
}

static uint16_t Groups(uint16_t fragments) { return static_cast<uint16_t>(std::min(32, std::max(1, (fragments + 9) / 10))); }

/// Sends one frame to the assembler: fragments not in `lost`, then all parity
/// (unless `parity` is false). Returns true if the frame came out, exactly.
static bool Deliver(FrameAssembler& a, uint32_t seq, const std::vector<uint8_t>& f, const std::set<size_t>& lost,
                    bool parity = true, bool parityFirst = false, AssembledFrame* outFrame = nullptr) {
    const uint16_t total = FragmentCount(f), groups = Groups(total);
    FrameAssembler::PacketInfo info{ false, true, 1000 };
    AssembledFrame out;
    bool done = false;
    auto sendParity = [&] {
        if (!parity) return;
        for (uint16_t g = 0; g < groups; g++) {
            std::vector<uint8_t> p = Parity(f, g, groups);
            done = a.addParity(seq, total, g, groups, static_cast<uint32_t>(f.size()), p.data(), p.size(), info, out) || done;
        }
    };
    if (parityFirst) sendParity();
    for (uint16_t i = 0; i < total; i++) {
        if (lost.count(i)) continue;
        std::vector<uint8_t> frag = Fragment(f, i);
        done = a.addFragment(seq, i, total, frag.data(), frag.size(), info, out) || done;
    }
    if (!parityFirst) sendParity();
    if (outFrame) *outFrame = out;
    return done && out.data == f;
}

static void TestReferenceVectors() {
    std::printf("Parity: the same bytes as the Mac (vectors shared with LossRepairTests.swift)\n");
    struct Case { size_t bytes, group, groups, length; uint32_t crc; };
    const Case cases[] = {
        { 4000, 0, 1, 1340, 0x2f7e00a7 }, { 30000, 0, 3, 1340, 0x2c3f2043 }, { 30000, 1, 3, 1340, 0x9b1dd341 },
        { 30000, 2, 3, 1340, 0x36fafe97 }, { 900, 0, 1, 900, 0x041117fd },
    };
    for (const Case& c : cases) {
        std::vector<uint8_t> p = Parity(Frame(c.bytes), c.group, c.groups);
        CHECK(p.size() == c.length);
        CHECK(exdp::computeCRC32(p.data(), p.size()) == c.crc);
    }
    exdp::ParityHeader h;
    h.frameSequence = 0x01020304;
    h.fragmentTotal = 23;
    h.groupIndex = 2;
    h.groupCount = 3;
    h.frameBytes = 30000;
    const uint8_t expected[16] = { 4, 3, 2, 1, 23, 0, 2, 0, 3, 0, 0, 0, 0x30, 0x75, 0, 0 };
    CHECK(std::memcmp(&h, expected, 16) == 0);
    CHECK(static_cast<int>(exdp::MessageType::VideoParity) == 0x42);
    CHECK(exdp::CAP_PARITY == 2 && exdp::CAP_PACING == 4);
}

static void TestAssembly() {
    std::printf("Assembly: complete frames, any order, duplicates, late parity\n");
    FrameAssembler a;
    CHECK(Deliver(a, 1, Frame(30000), {}));                       // no loss
    CHECK(a.fragmentsRebuilt() == 0 && a.incomplete() == 0);

    // Reversed order, every fragment twice: one frame, once
    std::vector<uint8_t> f = Frame(8000);
    const uint16_t total = FragmentCount(f);
    FrameAssembler::PacketInfo info{ true, true, 5 };
    AssembledFrame out;
    int completions = 0;
    for (int i = total - 1; i >= 0; i--) {
        std::vector<uint8_t> frag = Fragment(f, static_cast<size_t>(i));
        completions += a.addFragment(2, static_cast<uint16_t>(i), total, frag.data(), frag.size(), info, out);
        completions += a.addFragment(2, static_cast<uint16_t>(i), total, frag.data(), frag.size(), info, out);
    }
    CHECK(completions == 1 && out.data == f && out.keyframe && out.frameSequence == 2);
    // Parity arriving after the frame is done changes nothing
    std::vector<uint8_t> p = Parity(f, 0, 1);
    CHECK(!a.addParity(2, total, 0, 1, static_cast<uint32_t>(f.size()), p.data(), p.size(), info, out));
    CHECK(a.incomplete() == 0);
}

static void TestRepair() {
    std::printf("Repair: one loss per group, bursts up to the group count, the last fragment\n");
    std::vector<uint8_t> f = Frame(30001);                        // 23 fragments, last 521 bytes, 3 groups
    uint32_t seq = 100;
    bool all = true;
    for (size_t lost = 0; lost < 23; lost++) {
        FrameAssembler fresh;
        all = Deliver(fresh, seq++, f, { lost }) && all;
    }
    CHECK(all);                                                   // each single loss rebuilt (incl. the short last one)

    FrameAssembler a;
    AssembledFrame out;
    CHECK(Deliver(a, 1, f, { 5, 6, 7 }, true, false, &out));      // a burst of 3 = groups
    CHECK(out.rebuilt == 3 && a.fragmentsRebuilt() == 3 && a.framesRebuilt() == 1);
    CHECK(Deliver(a, 2, f, { 0, 4, 8 }));                         // one per group, spread out
    CHECK(Deliver(a, 3, f, { 22 }, true, true));                  // parity first, then the data

    std::vector<uint8_t> keyframe = Frame(150'000);               // 112 fragments, 12 groups
    CHECK(Deliver(a, 4, keyframe, { 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51 }));   // burst of 12

    std::vector<uint8_t> small = Frame(700);                      // one fragment: parity is a copy
    CHECK(Deliver(a, 5, small, { 0 }));
    CHECK(!Deliver(a, 6, f, { 3 }, false));                       // no parity: no repair
}

static void TestUnrepairable() {
    std::printf("Unrepairable: two losses in a group wait, then count as lost\n");
    FrameAssembler a;
    std::vector<uint8_t> f = Frame(30000);
    CHECK(!Deliver(a, 1, f, { 0, 3 }));                           // both in group 0 (3 groups)
    CHECK(a.incomplete() == 1 && a.dropOld() == 0);               // still within reach
    for (uint32_t s = 2; s <= 11; s++) CHECK(Deliver(a, s, Frame(500), {}));
    CHECK(a.dropOld() == 0);                                      // 10 behind: kept
    CHECK(Deliver(a, 12, Frame(500), {}));
    CHECK(a.dropOld() == 1);                                      // 11 behind: lost for good
    CHECK(a.incomplete() == 0);
}

static void TestHostilePackets() {
    std::printf("Hostile packets: inconsistent or impossible headers are ignored\n");
    FrameAssembler a;
    FrameAssembler::PacketInfo info{};
    AssembledFrame out;
    std::vector<uint8_t> f = Frame(4000), p = Parity(f, 0, 1), frag = Fragment(f, 0);
    CHECK(!a.addParity(1, 3, 0, 1, 2680, p.data(), p.size(), info, out));     // frameBytes too small for 3
    CHECK(!a.addParity(1, 3, 0, 1, 4021, p.data(), p.size(), info, out));     // too large
    CHECK(!a.addParity(1, 3, 2, 2, 4000, p.data(), p.size(), info, out));     // group ≥ groups
    CHECK(!a.addParity(1, 3, 0, 4, 4000, p.data(), p.size(), info, out));     // more groups than fragments
    CHECK(!a.addFragment(1, 3, 3, frag.data(), frag.size(), info, out));      // index ≥ total
    CHECK(!a.addFragment(1, 0, 0, frag.data(), frag.size(), info, out));
    std::vector<uint8_t> tooLong(kSize + 1, 1);
    CHECK(!a.addFragment(1, 0, 3, tooLong.data(), tooLong.size(), info, out));
    CHECK(!a.addFragment(1, 0, 3, frag.data(), frag.size(), info, out));      // accepted (not complete)...
    CHECK(a.incomplete() == 1);
    CHECK(!a.addFragment(1, 1, 4, frag.data(), frag.size(), info, out));      // ...but a different total is not
    // A parity that does not add up (claims another frame size) never builds a wrong fragment
    std::vector<uint8_t> f2 = Frame(30000), wrong = Parity(Frame(29000), 0, 3);
    FrameAssembler b;
    const uint16_t t2 = FragmentCount(f2);
    for (uint16_t i = 1; i < t2; i++) {
        std::vector<uint8_t> x = Fragment(f2, i);
        b.addFragment(9, i, t2, x.data(), x.size(), info, out);
    }
    CHECK(b.addParity(9, t2, 0, 3, 30000, wrong.data(), wrong.size(), info, out) && out.data != f2);
    // (XOR cannot tell a wrong parity from a right one: AES-GCM authenticates every packet first)
}

static void TestAdmission() {
    std::printf("Admission: very late frames ignored, renumbering and reset accepted\n");
    FrameAssembler a;
    CHECK(Deliver(a, 1000, Frame(500), {}));
    CHECK(!Deliver(a, 980, Frame(500), {}));                      // 20 behind: given up already
    CHECK(Deliver(a, 995, Frame(500), {}));                       // 5 behind: still fine
    CHECK(Deliver(a, 3, Frame(500), {}) == false);                // Mac restarted without START_STREAM yet...
    a.reset();                                                    // ...START_STREAM resets
    CHECK(Deliver(a, 3, Frame(500), {}));
    CHECK(Deliver(a, 4'000'000, Frame(500), {}));                 // a huge jump forward is a new numbering
    CHECK(Deliver(a, 5, Frame(500), {}));                         // and so is a huge jump back
}

static void TestLossSimulator() {
    std::printf("Loss simulator: rate, bursts, determinism\n");
    LossSimulator off;
    CHECK(!off.active() && !off.drop());

    LossSimulator s;
    s.configure(2.0, 1);
    int dropped = 0;
    for (int i = 0; i < 200000; i++) dropped += s.drop();
    CHECK(std::fabs(dropped / 2000.0 - 2.0) < 0.2);              // ≈ 2 %

    LossSimulator b;
    b.configure(4.0, 4);
    std::vector<int> runs;
    int run = 0;
    dropped = 0;
    for (int i = 0; i < 200000; i++) {
        if (b.drop()) { run++; dropped++; }
        else if (run) { runs.push_back(run); run = 0; }
    }
    bool multiplesOf4 = !runs.empty();
    for (int r : runs) multiplesOf4 = multiplesOf4 && r % 4 == 0;   // back-to-back events merge
    CHECK(multiplesOf4);
    CHECK(std::fabs(dropped / 2000.0 - 4.0) < 0.5);

    LossSimulator x, y;
    x.configure(3.0, 2, 42);
    y.configure(3.0, 2, 42);
    bool same = true;
    for (int i = 0; i < 10000; i++) same = same && x.drop() == y.drop();
    CHECK(same);
}

// ── Delay feedback (step 6) ─────────────────────────────────────────────────

/// Frames at 60 FPS from `startUs`; the Mac's clock is far off the PC's
/// (offset), the extra delay of each frame comes from `extraUs(i)`
template <typename Extra>
static void Feed(DelayMonitor& m, int64_t startUs, int frames, Extra extraUs, uint32_t bytes = 20000) {
    const int64_t offset = -7'000'000'000;           // Mac clock 7000 s ahead
    for (int i = 0; i < frames; i++) {
        const int64_t sentPc = startUs + i * 16'667;
        const int64_t arrival = sentPc + 2'000 + extraUs(i);   // 2 ms on the wire
        m.onPacket(static_cast<uint64_t>(sentPc - offset), arrival, bytes);
    }
}

static void TestDelayMonitor() {
    std::printf("Delay monitor: queuing delay from one-way delay, verdicts, rate, loss\n");
    DelayMonitor m;
    RateReport r;
    // Steady link: no queue, whatever the clock offset
    Feed(m, 1'000'000, 60, [](int) { return 0; });
    CHECK(m.report(2'000'000, 1000, 0, r));
    CHECK(r.state == RateState::Normal && r.queuingDelayUs < 500 && r.samples == 6);
    CHECK(std::abs(static_cast<int>(r.receivedKbps) - 30 * 20000 * 8 / 500) < 100);   // 30 frames of 20 kB in 500 ms
    CHECK(r.linkKbps == 0);                               // frames got through without waiting: the link had room
    // At a stream start the window is not full: the rate is over the time covered
    DelayMonitor early;
    Feed(early, 1'000'000, 12, [](int) { return 0; });     // 200 ms of frames
    CHECK(early.report(1'200'000, 100, 0, r));
    CHECK(std::abs(static_cast<int>(r.receivedKbps) - 12 * 20000 * 8 / 200) < 500);
    CHECK(!m.report(2'050'000, 1000, 0, r));              // every 100 ms only

    // A queue builds: +2 ms per frame → overuse within a few frames past 15 ms
    DelayMonitor q;
    Feed(q, 1'000'000, 60, [](int) { return 0; });
    Feed(q, 2'000'000, 12, [](int i) { return 2'000 * (i + 1); });   // up to 24 ms
    CHECK(q.report(2'200'000, 2000, 0, r));
    CHECK(r.state == RateState::Overuse && r.queuingDelayUs > 15'000);
    // Every frame of the last 100 ms waited: the link was full, what it carried is its capacity
    CHECK(r.linkKbps == 6 * 20000 * 8 / 100);
    // After the Mac cut the bitrate the queue drains: falling delay is not overuse
    Feed(q, 2'200'000, 6, [](int i) { return 20'000 - 3'000 * i; });  // 20 → 5 ms
    CHECK(q.report(2'300'000, 2100, 0, r));
    CHECK(r.state == RateState::Hold && r.queuingDelayUs > 5'000);
    Feed(q, 2'400'000, 6, [](int) { return 1'000; });
    CHECK(q.report(2'500'000, 2200, 0, r));
    CHECK(r.state == RateState::Normal);

    // A keyframe's burst: a long queue that empties again within 100 ms. Overuse,
    // but the link had room (no capacity figure: the Mac does not cut for it)
    DelayMonitor k;
    Feed(k, 1'000'000, 60, [](int) { return 0; });
    Feed(k, 2'000'000, 6, [](int i) { return i == 0 ? 0 : 40'000 - 8'000 * i; });   // 0, 32, 24, 16, 8, 0 ms
    CHECK(k.report(2'100'000, 1000, 0, r));
    CHECK(r.state == RateState::Overuse && r.linkKbps == 0);

    // One 40 ms spike among normal frames is filtered by the median
    DelayMonitor s;
    Feed(s, 1'000'000, 60, [](int) { return 0; });
    Feed(s, 2'000'000, 6, [](int i) { return i == 3 ? 40'000 : 500; });
    CHECK(s.report(2'100'000, 1000, 0, r) && r.state == RateState::Normal);

    // Loss over the last 500 ms, from the packet totals
    DelayMonitor l;
    Feed(l, 0, 60, [](int) { return 0; });
    CHECK(l.report(1'000'000, 1000, 0, r));
    CHECK(l.report(1'500'000, 1990, 10, r));
    CHECK(r.lossPercent100 == 100);                       // 10 of 1000 in the last 500 ms

    // A lasting route change (+20 ms for good) is the new normal after 10 s
    DelayMonitor b;
    Feed(b, 0, 60, [](int) { return 0; });
    Feed(b, 1'000'000, 60 * 11, [](int) { return 20'000; });
    CHECK(b.report(1'000'000 + 11 * 1'000'000, 5000, 0, r));
    CHECK(r.state == RateState::Normal && r.queuingDelayUs < 1'000);

    // Nothing arrived (static screen): no information, not a verdict
    DelayMonitor e;
    CHECK(e.report(5'000'000, 0, 0, r) && r.samples == 0);
    e.reset();
    CHECK(e.report(5'000'000, 0, 0, r));                  // reset: due again at once

    // Wire layout: identical to the Mac's test (testFeedbackLayoutMatchesWindows)
    exdp::RateFeedbackPayload p;
    p.queuingDelayUs = 0x01020304;
    p.receivedKbps = 8000;
    p.lossPercent100 = 125;
    p.state = 2;
    p.samples = 6;
    p.linkKbps = 7000;
    const uint8_t expected[16] = { 4, 3, 2, 1, 0x40, 0x1f, 0, 0, 0x7d, 0, 2, 6, 0x58, 0x1b, 0, 0 };
    CHECK(std::memcmp(&p, expected, 16) == 0);
    CHECK(static_cast<int>(exdp::MessageType::RateFeedback) == 0x52);
}

static void TestLinkSimulator() {
    std::printf("Link simulator: rate, queue, tail drop\n");
    LinkSimulator off;
    CHECK(!off.active());
    LinkSimulator link;
    link.configure(8000, 100);                            // 8 Mbps = 1 byte per µs, 100 ms queue
    int64_t exit = 0;
    CHECK(link.pass(0, 1000, exit) && exit == 1000);      // alone: just its own time on the link
    CHECK(link.pass(0, 1000, exit) && exit == 2000);      // behind the first
    CHECK(link.pass(5000, 1000, exit) && exit == 6000);   // the link was idle again
    // A burst far above 8 Mbps: queues up to 100 ms, then drops
    int passed = 0, dropped = 0;
    for (int i = 0; i < 200; i++) (link.pass(10'000, 1400, exit) ? passed : dropped)++;
    CHECK(passed > 70 && passed < 75 && dropped == 200 - passed);   // ~100 ms / 1.4 ms per packet

    // The receiver holds packets until they leave the link, in order
    LinkSimulator held;
    held.configure(8000, 100);
    const uint8_t a[3] = { 1, 2, 3 }, b[2] = { 4, 5 };
    CHECK(held.nextExitUs() == INT64_MAX);
    CHECK(held.hold(0, a, 3) && held.hold(0, b, 2));     // leave at 3 and 5 µs
    std::vector<uint8_t> out;
    CHECK(held.nextExitUs() == 3);
    CHECK(!held.release(2, out, exit));                   // still on the link
    CHECK(held.release(4, out, exit) && exit == 3 && out == std::vector<uint8_t>({ 1, 2, 3 }));
    CHECK(!held.release(4, out, exit));
    CHECK(held.release(100, out, exit) && exit == 5 && out == std::vector<uint8_t>({ 4, 5 }));
    CHECK(held.nextExitUs() == INT64_MAX);
    std::vector<uint8_t> big(1400, 7);
    int kept = 0;
    while (held.hold(1'000, big.data(), big.size())) kept++;
    CHECK(kept > 70 && kept < 75);                        // a full queue drops, as pass() does
    held.reset();                                         // a new connection: an empty link
    CHECK(held.nextExitUs() == INT64_MAX && held.hold(1'000, a, 3));
}

int main() {
    TestReferenceVectors();
    TestAssembly();
    TestRepair();
    TestUnrepairable();
    TestHostilePackets();
    TestAdmission();
    TestLossSimulator();
    TestDelayMonitor();
    TestLinkSimulator();
    std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
