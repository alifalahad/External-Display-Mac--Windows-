// Bitrate that follows the delay (docs/SMOOTHNESS.md, step 6): the delay
// controller's rules, the governor's choice between it and the tiers, and the
// RATE_FEEDBACK layout shared with windows-receiver/tests/NetworkTests.cpp

import XCTest
@testable import ExternalDisplay

final class RateControlTests: XCTestCase {

    /// Overuse comes from a full link unless linkKbps says otherwise: what it
    /// carried is then what arrived
    private func feedback(_ state: RateFeedbackPayload.State, receivedKbps: UInt32 = 20_000,
                          linkKbps: UInt32? = nil, loss100: UInt16 = 0, samples: UInt8 = 6) -> RateFeedbackPayload {
        var f = RateFeedbackPayload()
        f.state = state
        f.queuingDelayUs = state == .overuse ? 30_000 : 1_000
        f.receivedKbps = receivedKbps
        f.linkKbps = linkKbps ?? (state == .overuse ? receivedKbps : 0)
        f.lossPercent100 = loss100
        f.samples = samples
        return f
    }

    func testFeedbackLayoutMatchesWindows() {
        // queuingDelayUs 0x01020304, receivedKbps 8000, loss 1.25 %, overuse, 6 samples, link 7000 kbps
        let bytes = Data([0x04, 0x03, 0x02, 0x01, 0x40, 0x1f, 0x00, 0x00, 0x7d, 0x00, 0x02, 0x06, 0x58, 0x1b, 0, 0])
        let f = RateFeedbackPayload.deserialize(from: bytes)
        XCTAssertEqual(f?.queuingDelayUs, 0x01020304)
        XCTAssertEqual(f?.receivedKbps, 8000)
        XCTAssertEqual(f?.lossPercent100, 125)
        XCTAssertEqual(f?.state, .overuse)
        XCTAssertEqual(f?.samples, 6)
        XCTAssertEqual(f?.linkKbps, 7000)
        XCTAssertNil(RateFeedbackPayload.deserialize(from: Data(count: 15)))
        XCTAssertEqual(MessageType.rateFeedback.rawValue, 0x52)
    }

    func testOveruseCutsTo85PercentOfWhatTheFullLinkCarried() throws {
        let c = DelayRateController(preset: .balanced)            // starts at 20 Mbps
        let cut = try XCTUnwrap(c.onFeedback(feedback(.overuse, receivedKbps: 8_000), encoderBps: 20_000_000, now: 10))
        XCTAssertEqual(cut, 6_800_000, "85 % of the 8 Mbps the link carried, not of the 20 asked for")
        // The queue needs time to drain: no second cut within 0.3 s
        XCTAssertNil(c.onFeedback(feedback(.overuse, receivedKbps: 6_000), encoderBps: 6_800_000, now: 10.1))
        XCTAssertEqual(c.onFeedback(feedback(.overuse, receivedKbps: 6_000), encoderBps: 6_800_000, now: 10.35), 5_100_000)
    }

    func testQueueThatEmptiedAgainIsNoReasonToCut() {
        // A keyframe's burst: overuse, but frames got through without waiting.
        // What arrived (4 Mbps) is the encoder's output, not the link's capacity.
        let c = DelayRateController(preset: .balanced)
        XCTAssertNil(c.onFeedback(feedback(.overuse, receivedKbps: 4_000, linkKbps: 0), encoderBps: 4_000_000, now: 10))
        XCTAssertEqual(c.targetBps, QualityPreset.balanced.startBitrateBps)
        // Heavy loss still lowers the bitrate: a keyframe too large for the queue
        XCTAssertEqual(c.onFeedback(feedback(.overuse, receivedKbps: 4_000, linkKbps: 0, loss100: 2_000),
                                    encoderBps: 4_000_000, now: 10.1), 18_000_000)
    }

    func testCutCountsOnlyTheVideoShare() {
        // The receiver measures everything that arrived; 12 % of it was parity and headers
        let c = DelayRateController(preset: .balanced)
        let cut = c.onFeedback(feedback(.overuse, receivedKbps: 10_000), encoderBps: 20_000_000,
                               videoShare: 0.88, now: 10)
        XCTAssertEqual(cut, 7_480_000)                 // 0.85 × 10 Mbps × 0.88
    }

    func testFloorIsAQuarterOfThePresetMinimum() {
        let c = DelayRateController(preset: .balanced)
        var now = 0.0
        for _ in 0..<40 {
            now += 0.4
            _ = c.onFeedback(feedback(.overuse, receivedKbps: 100), encoderBps: 0, now: now)
        }
        XCTAssertEqual(c.targetBps, QualityPreset.balanced.minBitrateBps / 4)   // 2 Mbps
    }

    func testRisesSlowlyOnlyWhileUsed() {
        let c = DelayRateController(preset: .balanced)
        _ = c.onFeedback(feedback(.overuse, receivedKbps: 10_000), encoderBps: 20_000_000, now: 0)   // → 8.5 Mbps
        XCTAssertEqual(c.targetBps, 8_500_000)
        // Normal reports every 100 ms: nothing for 1 s after the cut, then ≈ 8 % per second
        var now = 0.0
        while now < 0.95 {
            now += 0.1
            _ = c.onFeedback(feedback(.normal), encoderBps: 8_500_000, now: now)
        }
        XCTAssertEqual(c.targetBps, 8_500_000)
        while now < 6.0 {
            now += 0.1
            _ = c.onFeedback(feedback(.normal), encoderBps: c.targetBps, now: now)
        }
        let perSecond = pow(Double(c.targetBps) / 8_500_000, 1 / 5.0) - 1
        XCTAssertEqual(perSecond, 0.08, accuracy: 0.01)
        // A static screen (encoder far below the target): nothing to probe, no rise
        let before = c.targetBps
        for _ in 0..<20 {
            now += 0.1
            _ = c.onFeedback(feedback(.normal), encoderBps: 1_000_000, now: now)
        }
        XCTAssertEqual(c.targetBps, before)
    }

    func testRecoversFastWhenFarBelowTheLastCapacity() {
        // A keyframe-sized dip cut the bitrate to 3.4 Mbps, but the link carried 10
        let c = DelayRateController(preset: .high)
        _ = c.onFeedback(feedback(.overuse, receivedKbps: 4_000), encoderBps: 25_000_000, now: 0)   // capacity 4 Mbps
        _ = c.onFeedback(feedback(.overuse, receivedKbps: 10_000), encoderBps: 3_400_000, now: 0.5) // capacity 10
        XCTAssertEqual(c.targetBps, 2_890_000)        // 85 % of the 3.4 asked for (received was higher)
        var now = 1.5
        while c.targetBps < 7_000_000 && now < 30 {
            now += 0.1
            _ = c.onFeedback(feedback(.normal), encoderBps: c.targetBps, now: now)
        }
        XCTAssertLessThan(now - 1.5, 4.5, "25 %/s below 70 % of the 10 Mbps found: 2.9 → 7 Mbps in ~4 s")
        let at7 = c.targetBps
        for _ in 0..<10 {
            now += 0.1
            _ = c.onFeedback(feedback(.normal), encoderBps: c.targetBps, now: now)
        }
        XCTAssertEqual(Double(c.targetBps) / Double(at7), 1.08, accuracy: 0.01, "then 8 %/s near the capacity")
    }

    func testHoldAndNoSignalChangeNothing() {
        let c = DelayRateController(preset: .high)
        XCTAssertNil(c.onFeedback(feedback(.hold), encoderBps: 25_000_000, now: 5))
        XCTAssertNil(c.onFeedback(feedback(.overuse, samples: 0), encoderBps: 25_000_000, now: 6))
        XCTAssertEqual(c.targetBps, QualityPreset.high.startBitrateBps)
    }

    func testHeavyLossLowersEvenWithoutDelay() {
        let c = DelayRateController(preset: .balanced)
        XCTAssertEqual(c.onFeedback(feedback(.normal, loss100: 2_000), encoderBps: 20_000_000, now: 1), 18_000_000,
                       "20 % loss: −10 %")
        XCTAssertNil(c.onFeedback(feedback(.normal, loss100: 300), encoderBps: 18_000_000, now: 3),
                     "3 % loss: hold, no rise")
    }

    func testMaximumAndSmallChanges() {
        let c = DelayRateController(preset: .batterySaver)      // 4–12 Mbps, starts at 8
        var now = 2.0
        var changes = 0
        for _ in 0..<400 {
            now += 0.1
            if c.onFeedback(feedback(.normal), encoderBps: c.targetBps, now: now) != nil { changes += 1 }
        }
        XCTAssertEqual(c.targetBps, QualityPreset.batterySaver.maxBitrateBps, "never above the preset")
        XCTAssertLessThan(changes, 20, "changes under 3 % are not sent to the encoder")
        // A preset change hands over a target held back as too small a step...
        let pending = c.apply(.balanced)
        XCTAssertTrue(pending == nil || pending == 12_000_000, "12 Mbps is inside Balanced")
        // ...after that, nothing is left to send
        XCTAssertNil(c.apply(.batterySaver))
    }

    func testGovernorFollowsDelayOnceFeedbackArrives() {
        let g = BitrateGovernor(preset: .balanced)
        var applied: [Int] = []
        g.onBitrateChange = { applied.append($0) }
        g.encoderOutputBps = { 20_000_000 }
        XCTAssertEqual(g.mode, "tiers")
        var bad = QualityReportPayload()
        bad.packetLossPercent = 600   // 6 %: the tiers would go down after 2 reports
        g.onQualityReport(bad)
        g.onQualityReport(bad)
        XCTAssertEqual(applied.count, 1, "tiers in charge before any RATE_FEEDBACK")

        g.onRateFeedback(feedback(.overuse, receivedKbps: 10_000), now: 100)
        XCTAssertEqual(g.mode, "delay")
        XCTAssertEqual(applied.last, 8_500_000)
        let count = applied.count
        g.onQualityReport(bad)
        g.onQualityReport(bad)
        XCTAssertEqual(applied.count, count, "quality reports no longer change the bitrate")

        g.reset()                                               // a new receiver
        XCTAssertEqual(g.mode, "tiers")
        XCTAssertEqual(applied.last, QualityPreset.balanced.startBitrateBps)
    }
}
