// =============================================================================
// ReceiverClient.cpp — The real Windows StreamReceiver against the Mac sender
// =============================================================================
// Integration test (run-loopback.sh, runs under Wine on the Mac or natively on
// Windows). Expectation per run:
//   pair      — new pairing: types the code from <code-file>, gets approved
//   paired    — must authenticate with the stored key, no code asked
//   impostor  — holds a wrong key for a known device ID: must be refused,
//               then pair again with a new identity
//   wrongcode — types a wrong code first: refused, new code, then pairs
//   rejected  — the Mac user rejects: no session, user told, no retries
// Streaming runs then check the encrypted stream: frames, timing prefix,
// H.264 start codes, clock sync, SENDER_STATS, nothing lost — and the pointer
// drawn here (docs/SMOOTHNESS.md, step 1): RECEIVER_CAPS asks for it, the Mac
// sends CURSOR_SHAPE / CURSOR_POSITION, ending with a scripted path. The
// paired run switches it off and on again (F4); wrongcode never asks.
// Keyframes (docs/SMOOTHNESS.md, step 2): the receiver's KeyframeGate runs on
// the real stream; 12 s in one frame is "lost" on purpose. The gate must
// notice the gap, ask at once, and the Mac's keyframe must come within
// 300 ms. No keyframes except the ones asked for.
// Colours (step 4): the keyframes' SPS must say full-range BT.709, which is
// what the receiver's shader then uses.
// Loss repair (step 5): parity and pacing are asked for (except lossy-nofec).
//   lossy        1 % of the packets dropped on purpose: parity must rebuild
//                nearly all of them, ≥ 97 % of the frames arrive
//   lossy-nofec  the same loss without parity or pacing, for comparison
// Paced runs: a frame's packets must arrive spread out, not in one burst.
// Bitrate that follows the delay (step 6): an 8 Mbps link with a 200 ms queue
// in front of the receiver, the Mac starting at 20 Mbps.
//   narrow        delay feedback: the Mac must go below the link within 3 s
//                 and keep the queue short
//   narrow-tiers  the old loss/RTT tiers, for comparison
//
//   ReceiverClient <host> <seconds> <code-file> <mode>
// =============================================================================

#include "Network/StreamReceiver.h"
#include "Input/Pointer.h"
#include "Decoder/KeyframeGate.h"
#include "Decoder/H264Sps.h"
#include "Security/PairingManager.h"
#include "Performance/PipelineMetrics.h"
#include "Performance/PipelineClock.h"
#include "App/Log.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <vector>

int main(int argc, char** argv) {
    std::string host = argc > 1 ? argv[1] : "127.0.0.1";
    int seconds = argc > 2 ? atoi(argv[2]) : 15;
    std::string codeFile = argc > 3 ? argv[3] : "code.txt";
    const std::string mode = argc > 4 ? argv[4] : "pair";
    const bool streams = mode != "rejected";
    const bool lossy = mode == "lossy" || mode == "lossy-nofec";
    const bool repair = mode != "lossy-nofec";             // parity + pacing asked for
    const bool narrow = mode == "narrow" || mode == "narrow-tiers";
    const bool drowning = mode == "narrow-tiers";   // the old tiers drown the narrow link: that is the comparison
    setvbuf(stdout, nullptr, _IONBF, 0);
    Log::Init("", LogLevel::Info);

    PairingManager pm;
    pm.initialize();
    if (mode == "impostor") pm.savePairing("Mac", std::vector<uint8_t>(32, 0x42));   // wrong key
    StreamReceiver rx;
    rx.setPairingManager(&pm);

    std::mutex m;
    std::vector<FrameTimeline> frames;
    size_t badStartCodes = 0, keyframes = 0;
    KeyframeGate gate;                       // as in ReceiverApp::decodePending
    int64_t firstFrameUs = 0, lostAtUs = 0, recoveryAskedUs = 0, recoveredUs = 0;
    uint64_t heldAfterLoss = 0;
    VideoColor streamColor;
    int keyframesWithSps = 0;
    rx.setFrameCallback([&](const uint8_t* d, size_t n, const FrameTimeline& t) {
        std::lock_guard<std::mutex> lock(m);
        if (n < 5 || d[0] != 0 || d[1] != 0 || d[2] != 0 || d[3] != 1) badStartCodes++;
        if (t.keyframe) keyframes++;
        if (t.keyframe && FindSpsColor(d, n, streamColor)) keyframesWithSps++;
        FrameTimeline copy = t;
        copy.decodeStartUs = copy.decodeEndUs = copy.presentUs = t.completeUs;   // no decoder here
        frames.push_back(copy);

        if (!firstFrameUs) firstFrameUs = t.completeUs;
        if (!lostAtUs && !t.keyframe && t.completeUs - firstFrameUs > 12'000'000) {
            lostAtUs = t.completeUs;         // never reaches the gate: lost on the way
            return;
        }
        const uint64_t heldBefore = gate.framesHeld();
        gate.admit(t.frameSequence, t.hasSenderTiming, t.keyframe, t.completeUs);
        if (lostAtUs && !recoveredUs) {
            heldAfterLoss += gate.framesHeld() - heldBefore;
            if (t.keyframe && recoveryAskedUs) recoveredUs = t.completeUs;
        }
    });
    bool streamStarted = false;
    rx.setStreamInfoCallback([&](uint32_t w, uint32_t h, uint32_t fps) {
        std::printf("[Client] START_STREAM %ux%u @ %u\n", w, h, fps);
        std::lock_guard<std::mutex> lock(m);
        gate.reset();                        // new stream, new decoder
        streamStarted = true;
    });

    // Pointer drawn here: every message must parse
    const bool askPointer = mode != "wrongcode";
    std::vector<PointerShape> shapes;
    std::vector<PointerPosition> positions;
    int malformedPointer = 0;
    int pointerRequests = askPointer ? 1 : 0;
    rx.setReceiverCaps((askPointer ? exdp::CAP_LOCAL_CURSOR : 0) |
                       (repair ? exdp::CAP_PARITY | exdp::CAP_PACING : 0));
    if (lossy) rx.simulateLoss(1.0, 1);
    if (narrow) rx.simulateLink(8000, 200);
    rx.enableRateFeedback(mode != "narrow-tiers");
    struct RateSample { double sinceStartS, targetMbps, queueMs; uint64_t lost; };
    std::vector<RateSample> rateSamples;
    int64_t lastRateSampleUs = 0;
    rx.setPointerCallbacks(
        [&](const uint8_t* d, size_t n) {
            std::lock_guard<std::mutex> lock(m);
            PointerShape shape;
            if (ParsePointerShape(d, n, shape)) shapes.push_back(std::move(shape)); else malformedPointer++;
        },
        [&](const uint8_t* d, size_t n) {
            std::lock_guard<std::mutex> lock(m);
            PointerPosition p;
            if (ParsePointerPosition(d, n, p)) positions.push_back(p); else malformedPointer++;
        });
    rx.connect(host);

    int codeRequests = 0;
    int64_t streamStartUs = 0;
    int pointerToggle = 0;
    int64_t end = PipelineClock::NowUs() + (int64_t)seconds * 1000000;
    while (PipelineClock::NowUs() < end) {
        uint32_t req = 0;
        if (rx.takeCodeRequest(req)) {
            codeRequests++;
            std::string code;
            for (int i = 0; i < 100 && code.empty(); i++) {
                std::ifstream f(codeFile);
                std::getline(f, code);
                if (code.empty()) Sleep(50);
            }
            std::remove(codeFile.c_str());   // the next request must wait for a new code
            if (mode == "wrongcode" && codeRequests == 1 && code.size() == 6) {
                code[5] = code[5] == '9' ? '0' : static_cast<char>(code[5] + 1);   // one digit off
            }
            auto result = rx.submitVerificationCode(req, code);
            std::printf("[Client] typed the code: %s\n",
                        result == StreamReceiver::CodeResult::Sent ? "verified"
                        : result == StreamReceiver::CodeResult::Mismatch ? "mismatch (refused)" : "expired");
        }
        // What the Mac aims for and how long the queue is, every 100 ms
        if (streamStartUs && PipelineClock::NowUs() - lastRateSampleUs >= 100'000) {
            lastRateSampleUs = PipelineClock::NowUs();
            const SenderSample sample = rx.senderStats();
            const auto now = rx.getStats();
            rateSamples.push_back({ (lastRateSampleUs - streamStartUs) / 1e6, sample.valid ? sample.targetMbps : 0,
                                    now.queuingDelayUs / 1000.0, now.packetsLost });
        }
        // Ask while the gate waits for a keyframe: at once, then every 250 ms
        bool askKeyframe = false;
        {
            std::lock_guard<std::mutex> lock(m);
            askKeyframe = streamStarted && gate.shouldRequestKeyframe(PipelineClock::NowUs());
            if (askKeyframe && lostAtUs && !recoveryAskedUs) recoveryAskedUs = PipelineClock::NowUs();
        }
        if (askKeyframe) rx.requestKeyframe();
        // Paired run: F4 off and on again, sent at once over the running session
        if (streamStarted && !streamStartUs) streamStartUs = PipelineClock::NowUs();
        const int64_t sinceStartUs = streamStartUs ? PipelineClock::NowUs() - streamStartUs : 0;
        if (mode == "paired" && pointerToggle == 0 && sinceStartUs > 5'000'000) {
            rx.setReceiverCaps(exdp::CAP_PARITY | exdp::CAP_PACING);
            pointerToggle = 1;
        } else if (mode == "paired" && pointerToggle == 1 && sinceStartUs > 6'000'000) {
            rx.setReceiverCaps(exdp::CAP_LOCAL_CURSOR | exdp::CAP_PARITY | exdp::CAP_PACING);
            pointerToggle = 2;
            pointerRequests++;
        }
        Sleep(20);
    }

    ClockSync::Estimate sync = rx.clockEstimate();
    SenderSample mac = rx.senderStats();
    StreamReceiver::Stats st = rx.getStats();
    bool secure = rx.isSecure();
    std::string userMessage = rx.takeUserMessage();
    auto finalState = rx.getState();
    rx.disconnect();

    RollingSamples encode(100000), network(100000);
    size_t withTiming = 0;
    for (auto& t : frames) {
        if (!t.hasSenderTiming) continue;
        withTiming++;
        StageTimes s = ComputeStageTimes(t, sync);
        if (s[(size_t)Stage::Encode] >= 0) encode.add(s[(size_t)Stage::Encode]);
        if (s[(size_t)Stage::Network] >= 0) network.add(s[(size_t)Stage::Network]);
    }
    std::printf("[Client] RESULT frames=%zu timing=%zu keyframes=%zu badStartCodes=%zu expected=%llu "
                "lost=%llu codeRequests=%d secure=%d paired=%d\n",
                frames.size(), withTiming, keyframes, badStartCodes, (unsigned long long)st.framesExpected,
                (unsigned long long)st.packetsLost, codeRequests, secure, pm.isPaired());
    std::printf("[Client] encode avg %.2f ms, network avg %.2f ms, RTT %.2f ms, clock +/-%.2f ms, "
                "Mac stats %s (encode %.0f FPS, HW %d)\n",
                encode.summary().avgMs, network.summary().avgMs, sync.smoothedRttMs, sync.errorBoundMs,
                mac.valid ? "ok" : "missing", mac.encodeFps, mac.hardwareEncoder);

    // The scripted path: the last 101 positions (diagonal, then off-display)
    bool scriptedPath = positions.size() >= 101 && !positions.back().visible;
    for (size_t i = 0; scriptedPath && i < 100; i++) {
        const PointerPosition& p = positions[positions.size() - 101 + i];
        const float t = static_cast<float>(i) / 99.0f;
        scriptedPath = p.visible && std::fabs(p.x - t) < 1e-6f && std::fabs(p.y - t) < 1e-6f;
    }
    bool shapesDrawable = !shapes.empty();
    for (const PointerShape& s : shapes) {
        bool opaque = false;
        for (size_t i = 3; i < s.bgra.size(); i += 4) opaque = opaque || s.bgra[i] == 255;
        shapesDrawable = shapesDrawable && opaque && s.scale > 0;
    }
    if (!shapes.empty()) {
        const PointerShape& last = shapes.back();
        std::printf("[Client] pointer: %zu shapes (last %dx%d, hot spot %d,%d, scale %.2f), %zu positions, "
                    "%d malformed, asked %d time(s)\n", shapes.size(), last.width, last.height,
                    last.hotspotX, last.hotspotY, last.scale, positions.size(), malformedPointer, pointerRequests);
    } else {
        std::printf("[Client] pointer: no shapes, %zu positions, asked %d time(s)\n", positions.size(), pointerRequests);
    }

    // Pacing: how long a frame of 10+ packets takes to arrive, first to last packet
    double spreadSumUs = 0;
    size_t spreadFrames = 0;
    for (const FrameTimeline& t : frames) {
        if (t.bytes < 10 * exdp::MAX_FRAGMENT_PAYLOAD) continue;
        spreadSumUs += static_cast<double>(t.completeUs - t.firstPacketUs);
        spreadFrames++;
    }
    const double spreadUs = spreadFrames ? spreadSumUs / spreadFrames : 0;
    const double lossPercent = st.packetsReceived + st.packetsLost
        ? 100.0 * st.packetsLost / (st.packetsReceived + st.packetsLost) : 0;
    const double delivered = st.framesExpected ? 100.0 * frames.size() / st.framesExpected : 0;
    std::printf("[Client] loss repair: %.2f%% of packets lost, %llu rebuilt from %llu parity packets "
                "(%llu frames saved), %.1f%% of frames arrived; Mac paces frames over %.0f us "
                "(10+ packet frames arrive here over %.0f us)\n",
                lossPercent, (unsigned long long)st.fragmentsRebuilt, (unsigned long long)st.parityPackets,
                (unsigned long long)st.framesRebuilt, delivered, mac.pacingSpreadUs, spreadUs);
    double belowLinkS = -1, queueSum = 0, startTarget = 0, endTarget = 0;
    size_t queueN = 0;
    uint64_t lostAtStart = 0;                            // packets lost in the first 2 s
    for (const RateSample& r : rateSamples) {
        if (r.sinceStartS <= 2.0) lostAtStart = r.lost;
        if (r.targetMbps > 0 && startTarget == 0) startTarget = r.targetMbps;
        if (r.targetMbps > 0) endTarget = r.targetMbps;
        if (belowLinkS < 0 && r.targetMbps > 0 && r.targetMbps <= 8.0) belowLinkS = r.sinceStartS;
        if (r.sinceStartS >= seconds - 12) { queueSum += r.queueMs; queueN++; }
    }
    const double queueAvgMs = queueN ? queueSum / queueN : 0;
    if (narrow) {
        std::string timeline;
        double nextS = 0;
        for (const RateSample& r : rateSamples) {
            if (r.sinceStartS < nextS) continue;
            char item[48];
            snprintf(item, sizeof(item), " %.0fs:%.1f/%.0fms", r.sinceStartS, r.targetMbps, r.queueMs);
            timeline += item;
            nextS += 2;
        }
        std::printf("[Client] timeline (s:Mbps/queue):%s\n", timeline.c_str());
        std::printf("[Client] narrow link 8 Mbps: Mac target %.1f -> %.1f Mbps, below the link after %.1f s; "
                    "queue %.1f ms on average in the last 10 s; %.1f%% of frames arrived; "
                    "packets lost: %llu in the first 2 s, %llu after\n",
                    startTarget, endTarget, belowLinkS, queueAvgMs, delivered, (unsigned long long)lostAtStart,
                    (unsigned long long)(st.packetsLost - lostAtStart));
    }
    std::printf("[Client] colours: %d of %zu keyframes carry an SPS, it says %s\n",
                keyframesWithSps, keyframes, keyframesWithSps ? VideoColorName(streamColor, 720) : "-");
    const int64_t recoveryUs = recoveredUs && recoveryAskedUs ? recoveredUs - recoveryAskedUs : -1;
    std::printf("[Client] keyframes: %zu received, %llu asked for; frame lost on purpose: %s, "
                "keyframe after %.1f ms, %llu frames held meanwhile\n",
                keyframes, (unsigned long long)gate.requests(), lostAtUs ? "yes" : "no",
                recoveryUs >= 0 ? recoveryUs / 1000.0 : -1.0, (unsigned long long)heldAfterLoss);

    int failures = 0;
    auto expect = [&](bool c, const char* what) { if (!c) { failures++; std::printf("  FAIL: %s\n", what); } };
    if (mode == "rejected") {
        expect(!secure, "no session after the Mac rejected");
        expect(frames.empty(), "no video without authentication");
        expect(codeRequests == 1, "one code asked");
        expect(userMessage.find("rejected") != std::string::npos, "user told the pairing was rejected");
        expect(finalState == StreamReceiver::State::Disconnected, "stopped retrying");
        std::printf("[Client] %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
        return failures ? 1 : 0;
    }
    expect(secure, "session authenticated and encrypted");
    expect(pm.isPaired(), "pairing key stored");
    if (mode == "pair" || mode == "impostor") expect(codeRequests == 1, "exactly one verification code asked");
    if (mode == "paired") expect(codeRequests == 0, "no code asked (already paired)");
    if (mode == "wrongcode") expect(codeRequests == 2, "wrong code refused, a new code issued");
    // A drowning link delivers few frames, and keyframes (the largest) rarely complete
    if (!drowning) {
        expect(streams && frames.size() > (size_t)(seconds - 8) * 40, "encrypted frames received (~60 FPS)");
        expect(keyframes >= 2, "keyframes on request: stream start and after the lost frame");
    }
    expect(withTiming == frames.size(), "every frame carries the timing prefix");
    expect(badStartCodes == 0, "decrypted frames start with an H.264 start code");
    // Every KEYFRAME_REQ the receiver sent (the gate's, and its own after a lost frame)
    expect(keyframes <= st.keyframeRequests + 1, "no periodic keyframes (one per second before)");
    if (!lossy && !narrow) {   // under simulated loss the gate is busy with other gaps
        expect(lostAtUs > 0 && recoveryAskedUs > lostAtUs && recoveryAskedUs - lostAtUs < 100'000,
               "the missing frame was noticed and a keyframe asked for at once");
        expect(recoveryUs >= 0 && recoveryUs < 300'000, "the Mac's keyframe arrived within 300 ms");
    }
    expect(keyframesWithSps == static_cast<int>(keyframes), "every keyframe starts with an SPS");
    if (!drowning || keyframesWithSps > 0) {
        expect(streamColor.fullRange && streamColor.matrix == 1, "the stream says full-range BT.709 (what the shader uses)");
    }
    if (!lossy && !narrow) expect(st.packetsLost == 0, "no packet lost or rejected on localhost");
    if (mode == "narrow") {
        expect(belowLinkS >= 0 && belowLinkS < 3.0, "the Mac went below the 8 Mbps link within 3 s");
        expect(queueN > 0 && queueAvgMs < 40, "the queue on the link stays short (under 40 ms on average)");
        expect(delivered >= 90, "at least 90 % of the frames arrived through the narrow link");
    }
    // Pacing is measured where it happens: the Mac reports it in SENDER_STATS
    if (repair) {
        expect(st.parityPackets > 0, "parity packets arrive (asked for in RECEIVER_CAPS)");
        // (On the narrow link the bitrate is low: frames of up to 8 packets are not paced)
        if (!narrow) expect(mac.pacingSpreadUs > 0 && mac.pacingSpreadUs < 5000, "the Mac paces frames (reported spread)");
        else expect(mac.pacingSpreadUs < 5000, "pacing spread stays small");
    } else {
        expect(st.parityPackets == 0, "no parity packets unless asked for");
        expect(mac.pacingSpreadUs == 0, "no pacing unless asked for");
    }
    if (lossy) {
        expect(lossPercent > 0.5 && lossPercent < 1.6, "about 1 % of the packets were dropped");
        if (repair) {
            expect(st.fragmentsRebuilt >= st.packetsLost * 8 / 10, "parity rebuilt at least 80 % of the lost packets");
            expect(delivered >= 97, "at least 97 % of the frames arrived despite the loss");
        } else {
            expect(st.fragmentsRebuilt == 0, "nothing rebuilt without parity");
        }
    }
    expect(sync.valid && sync.smoothedRttMs < 50, "clock sync converged over the encrypted channel");
    // The message path, not speed: a busy test Mac can encode slowly for a moment
    expect(mac.valid && mac.encodeFps > 0, "SENDER_STATS received (encrypted control message)");
    expect(malformedPointer == 0, "every pointer message parses");
    if (askPointer) {
        // Per request: the real monitor's image and the scripted arrow
        expect(shapes.size() >= static_cast<size_t>(2 * pointerRequests), "pointer images for every request");
        expect(shapesDrawable, "pointer images have an opaque body");
        expect(scriptedPath, "scripted pointer path arrived complete and in order");
    } else {
        expect(shapes.empty() && positions.empty(), "no pointer messages unless asked for");
    }
    std::printf("[Client] %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
