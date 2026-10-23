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
// H.264 start codes, clock sync, SENDER_STATS, nothing lost.
//
//   ReceiverClient <host> <seconds> <code-file> <mode>
// =============================================================================

#include "Network/StreamReceiver.h"
#include "Security/PairingManager.h"
#include "Performance/PipelineMetrics.h"
#include "Performance/PipelineClock.h"
#include "App/Log.h"

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
    rx.setFrameCallback([&](const uint8_t* d, size_t n, const FrameTimeline& t) {
        std::lock_guard<std::mutex> lock(m);
        if (n < 5 || d[0] != 0 || d[1] != 0 || d[2] != 0 || d[3] != 1) badStartCodes++;
        if (t.keyframe) keyframes++;
        FrameTimeline copy = t;
        copy.decodeStartUs = copy.decodeEndUs = copy.presentUs = t.completeUs;   // no decoder here
        frames.push_back(copy);
    });
    bool streamStarted = false;
    rx.setStreamInfoCallback([&](uint32_t w, uint32_t h, uint32_t fps) {
        std::printf("[Client] START_STREAM %ux%u @ %u\n", w, h, fps);
        streamStarted = true;
    });
    rx.connect(host);

    int codeRequests = 0;
    bool askedKeyframe = false;
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
        if (streamStarted && !askedKeyframe) { rx.requestKeyframe(); askedKeyframe = true; }
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
    expect(streams && frames.size() > (size_t)(seconds - 8) * 40, "encrypted frames received (~60 FPS)");
    expect(withTiming == frames.size(), "every frame carries the timing prefix");
    expect(badStartCodes == 0, "decrypted frames start with an H.264 start code");
    expect(keyframes >= 1, "keyframe received");
    expect(st.packetsLost == 0, "no packet lost or rejected on localhost");
    expect(sync.valid && sync.smoothedRttMs < 50, "clock sync converged over the encrypted channel");
    expect(mac.valid && mac.encodeFps > 30, "SENDER_STATS received (encrypted control message)");
    std::printf("[Client] %s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
