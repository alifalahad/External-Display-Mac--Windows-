// Adaptive quality, presets, rolling statistics, input mapping (Phase 20)

import XCTest
@testable import ExternalDisplay

final class AdaptiveQualityTests: XCTestCase {

    private func report(_ c: AdaptiveQualityController, loss: UInt32 = 0, rtt: UInt32 = 5,
                        drops: UInt32 = 0, queue: UInt32 = 0) {
        c.processReport(packetLossPercent100: loss, rttMs: rtt, framesDropped: drops, queueDepth: queue)
    }

    func testStepsUpOnlyAfterThreeGoodReports() {
        let c = AdaptiveQualityController(preset: .high)
        var changes: [Int] = []
        c.onBitrateChange = { changes.append($0) }
        report(c); report(c)
        XCTAssertTrue(changes.isEmpty, "hysteresis: no change after two reports")
        report(c)
        XCTAssertEqual(changes, [30_000_000], "25 → 30 Mbps (5 Mbps max step)")
    }

    func testStepsDownFasterThanUp() {
        let c = AdaptiveQualityController(preset: .balanced)
        var changes: [Int] = []
        c.onBitrateChange = { changes.append($0) }
        report(c, loss: 800); report(c, loss: 800)   // 8% loss: poor
        XCTAssertEqual(changes, [15_000_000], "20 → 15 Mbps after two bad reports")
    }

    func testNeverLeavesThePresetRange() {
        let c = AdaptiveQualityController(preset: .batterySaver)
        for _ in 0..<30 { report(c) }
        XCTAssertEqual(c.currentBitrateBps, QualityPreset.batterySaver.maxBitrateBps)
        for _ in 0..<30 { report(c, rtt: 300) }
        XCTAssertEqual(c.currentBitrateBps, QualityPreset.batterySaver.minBitrateBps)
    }

    func testPresetChangeClampsImmediately() {
        let c = AdaptiveQualityController(preset: .high)
        var changes: [Int] = []
        c.onBitrateChange = { changes.append($0) }
        for _ in 0..<12 { report(c) }                  // climb to 40 Mbps
        XCTAssertEqual(c.currentBitrateBps, 40_000_000)
        c.apply(.balanced)
        XCTAssertEqual(c.currentBitrateBps, 25_000_000)
        XCTAssertEqual(changes.last, 25_000_000)
    }

    func testRealRttMattersNow() {
        // Phase 16 made RTT real; a congested link (60 ms) must not count as excellent
        let c = AdaptiveQualityController(preset: .high)
        XCTAssertLessThan(c.bitrateForTier(.fair), c.bitrateForTier(.excellent))
        for _ in 0..<6 { report(c, rtt: 60) }
        XCTAssertLessThan(c.currentBitrateBps, 25_000_000)
    }
}

final class RollingStatsTests: XCTestCase {

    func testSummaryPercentiles() {
        var w = RollingWindow(capacity: 100)
        for i in 1...100 { w.add(Double(i)) }
        let s = w.summary()
        XCTAssertEqual(s.samples, 100)
        XCTAssertEqual(s.avgMs, 50.5, accuracy: 1e-9)
        XCTAssertEqual(s.p95Ms, 95)
        XCTAssertEqual(s.maxMs, 100)
        w.add(1000)                                     // evicts the oldest
        XCTAssertEqual(w.summary().samples, 100)
        XCTAssertEqual(w.summary().maxMs, 1000)
    }

    func testRateMeterWindow() {
        var r = RateMeter()
        for i in 0..<60 { r.record(at: 10 + Double(i) / 60, bytes: 1000) }
        XCTAssertEqual(r.rate(now: 10.99), 60, accuracy: 1)
        XCTAssertEqual(r.megabitsPerSecond(now: 10.99), 0.48, accuracy: 0.01)
        XCTAssertEqual(r.rate(now: 20), 0, "static screen: drops to zero")
    }
}

final class InputMappingTests: XCTestCase {

    func testCoordinatesStayOnTheStreamedDisplay() {
        let injector = InputInjector()
        injector.displayWidth = 1920
        injector.displayHeight = 1080
        injector.displayOriginX = 1440          // virtual display right of the MacBook screen
        injector.displayOriginY = 0
        XCTAssertEqual(injector.normalizedToDisplay(x: 0, y: 0), CGPoint(x: 1440, y: 0))
        XCTAssertEqual(injector.normalizedToDisplay(x: 1, y: 1), CGPoint(x: 1440 + 1919, y: 1079))
        XCTAssertEqual(injector.normalizedToDisplay(x: 0.5, y: 0.5), CGPoint(x: 1440 + 959.5, y: 539.5))
        // Out of range or garbage never reaches the MacBook's own screen
        XCTAssertEqual(injector.normalizedToDisplay(x: -3, y: 7), CGPoint(x: 1440, y: 1079))
        XCTAssertEqual(injector.normalizedToDisplay(x: .nan, y: .infinity), CGPoint(x: 1440, y: 1079))
    }

    func testWindowsKeyCodes() {
        let injector = InputInjector()
        XCTAssertEqual(injector.windowsVKToMacKeyCode(0x41), 0x00)     // A → kVK_ANSI_A
        XCTAssertEqual(injector.windowsVKToMacKeyCode(0x0D), 0x24)     // Enter → kVK_Return
        XCTAssertEqual(injector.windowsVKToMacKeyCode(0x1B), 0x35)     // Esc → kVK_Escape
        XCTAssertNil(injector.windowsVKToMacKeyCode(0xFF))
    }
}
