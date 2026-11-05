// 120 Hz (docs/SMOOTHNESS.md, step 7): the frame rate the stream runs at and
// why, when the encoder counts as too slow, and the refresh rate in
// RECEIVER_CAPS (bytes shared with windows-receiver/tests/PointerTests.cpp)

import XCTest
import CoreMedia
import CoreVideo
@testable import ExternalDisplay

final class FrameRateTests: XCTestCase {

    private func fps(_ preset: QualityPreset, pc: UInt32, display: Double = 120, keepsUp: Bool = true) -> Int {
        FrameRate.choose(preset: preset, receiverRefreshHz: pc, displayRefreshHz: display, encoderKeepsUp: keepsUp)
    }

    private func note(_ preset: QualityPreset, pc: UInt32, display: Double = 120, keepsUp: Bool = true) -> String {
        FrameRate.describe(preset: preset, receiverRefreshHz: pc, displayRefreshHz: display, encoderKeepsUp: keepsUp)
    }

    func testHighStreamsWhatThePCScreenShows() {
        XCTAssertEqual(fps(.high, pc: 120), 120)
        XCTAssertEqual(fps(.high, pc: 119), 120, "119.88 Hz is reported as 119")
        XCTAssertEqual(fps(.high, pc: 144), 120, "120 at most")
        XCTAssertEqual(fps(.high, pc: 60), 60)
        XCTAssertEqual(fps(.high, pc: 75), 60)
        XCTAssertEqual(fps(.high, pc: 48), 60, "a 48 Hz power-saving panel: 60 shows better than 30")
        XCTAssertEqual(fps(.high, pc: 30), 30, "a 4K TV at 30 Hz, or --max-fps=30")
        XCTAssertEqual(fps(.high, pc: 0), 60, "an older receiver never says")
    }

    func testOtherPresetsKeepTheirRate() {
        XCTAssertEqual(fps(.balanced, pc: 120), 60)
        XCTAssertEqual(fps(.batterySaver, pc: 120), 30)
        XCTAssertEqual(fps(.batterySaver, pc: 120, keepsUp: false), 30)
        XCTAssertEqual(QualityPreset.high.maxFps, 120)
        XCTAssertEqual(QualityPreset.high.summary, "up to 120 FPS · up to 40 Mbps")
        XCTAssertEqual(QualityPreset.balanced.summary, "60 FPS · up to 25 Mbps")
        XCTAssertEqual(ScreenCaptureEngine.displayRefreshHz(for: .high), 120, "the virtual display at 120 Hz only for High")
        XCTAssertEqual(ScreenCaptureEngine.displayRefreshHz(for: .balanced), 60)
    }

    func testDisplayAndEncoderLimits() {
        XCTAssertEqual(fps(.high, pc: 120, display: 60), 60, "a 60 Hz display delivers no more")
        XCTAssertEqual(fps(.high, pc: 120, display: 0), 60, "refresh rate unknown")
        XCTAssertEqual(fps(.high, pc: 120, display: 119.88), 120)
        XCTAssertEqual(fps(.high, pc: 120, keepsUp: false), 60)
    }

    func testTheWindowSaysWhy() {
        XCTAssertEqual(note(.high, pc: 120), "120 FPS")
        XCTAssertEqual(note(.balanced, pc: 120), "60 FPS — High streams 120 to this PC's 120 Hz screen")
        XCTAssertEqual(note(.balanced, pc: 60), "60 FPS")
        XCTAssertEqual(note(.high, pc: 60), "60 FPS — the PC's screen runs at 60 Hz")
        XCTAssertEqual(note(.high, pc: 0), "60 FPS — the PC has not said its screen's refresh rate")
        XCTAssertEqual(note(.high, pc: 120, display: 60), "60 FPS — the captured display refreshes at 60 Hz")
        XCTAssertEqual(note(.high, pc: 120, keepsUp: false), "60 FPS — the encoder cannot keep up with 120 at this size")
    }

    private let interval = 1000.0 / 120

    private func second(_ load: inout EncoderLoad, frames: Int, callMs: Double, latencyMs: Double) -> (behind: Bool, last: EncoderLoad.Second) {
        for _ in 0..<frames {
            load.recordCall(ms: callMs, intervalMs: interval)
            load.recordOutput(latencyMs: latencyMs, intervalMs: interval)
        }
        return load.secondElapsed()
    }

    func testEncoderThatBlocksTooLongFallsBehindAfterTwoSeconds() {
        // Measured on an M2 at 120 FPS: 1920×1200 blocks the call ~7.2 ms (inside
        // the 8.3 ms), 2560×1440 ~10.1 ms (only 92 frames a second get through)
        var load = EncoderLoad()
        XCTAssertFalse(second(&load, frames: 119, callMs: 7.2, latencyMs: 7.0).behind)
        XCTAssertFalse(second(&load, frames: 119, callMs: 7.2, latencyMs: 7.0).behind)
        let first = second(&load, frames: 92, callMs: 10.1, latencyMs: 10.0)
        XCTAssertFalse(first.behind, "one bad second could be a hiccup")
        XCTAssertEqual(first.last.slowShare, 1)
        XCTAssertEqual(first.last.avgCallMs, 10.1, accuracy: 1e-9)
        XCTAssertTrue(second(&load, frames: 92, callMs: 10.1, latencyMs: 10.0).behind)
    }

    func testLateOutputCountsToo() {
        // 1920×1080 on an M2: the call returns at once, output after ~8.1 ms — keeps up
        var ok = EncoderLoad()
        XCTAssertFalse(second(&ok, frames: 120, callMs: 0.1, latencyMs: 8.2).behind)
        XCTAssertFalse(second(&ok, frames: 120, callMs: 0.1, latencyMs: 8.2).behind)
        // A queue inside the encoder: frames come out 30 ms after going in
        var queued = EncoderLoad()
        XCTAssertFalse(second(&queued, frames: 120, callMs: 0.1, latencyMs: 30).behind)
        XCTAssertTrue(second(&queued, frames: 120, callMs: 0.1, latencyMs: 30).behind)
    }

    func testAStillScreenGivesNoVerdict() {
        var load = EncoderLoad()
        XCTAssertFalse(second(&load, frames: 92, callMs: 10.1, latencyMs: 10).behind)
        // 10 frames: too few to judge, the count of bad seconds stays
        XCTAssertFalse(second(&load, frames: 10, callMs: 10.1, latencyMs: 10).behind)
        XCTAssertTrue(second(&load, frames: 92, callMs: 10.1, latencyMs: 10).behind)
        // A good second ends the run
        var again = EncoderLoad()
        _ = second(&again, frames: 92, callMs: 10.1, latencyMs: 10)
        _ = second(&again, frames: 119, callMs: 2, latencyMs: 2)
        XCTAssertFalse(second(&again, frames: 92, callMs: 10.1, latencyMs: 10).behind)
        again.reset()
        XCTAssertEqual(again.secondElapsed().last, EncoderLoad.Second())
    }

    func testReceiverCapsCarryTheRefreshRate() {
        // Pointer drawn here, parity, pacing; a 120 Hz screen
        let caps = ReceiverCapsPayload.deserialize(from: Data([0x07, 0, 0, 0, 0x78, 0, 0, 0]))
        XCTAssertEqual(caps?.flags, 7)
        XCTAssertEqual(caps?.refreshHz, 120)
        XCTAssertEqual(ReceiverCapsPayload.deserialize(from: Data([1, 0, 0, 0]))?.refreshHz, 0,
                       "no refresh rate in a 4-byte payload")
    }

    func testEncoderSwitchesTo120WithoutANewSession() throws {
        var config = EncoderConfig()
        config.width = 640
        config.height = 360
        let encoder = try VideoEncoder(config: config)
        encoder.updateFrameRate(120)
        let frameCount = 60
        let done = expectation(description: "frames")
        done.expectedFulfillmentCount = frameCount
        encoder.onEncodedFrame = { _, _, _ in done.fulfill() }

        var pool: CVPixelBufferPool?
        let attrs: [CFString: Any] = [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
                                      kCVPixelBufferWidthKey: 640, kCVPixelBufferHeightKey: 360,
                                      kCVPixelBufferIOSurfacePropertiesKey: [:] as [String: Any]]
        CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool)
        for i in 0..<frameCount {
            var pb: CVPixelBuffer?
            CVPixelBufferPoolCreatePixelBuffer(nil, pool!, &pb)
            CVPixelBufferLockBaseAddress(pb!, [])
            let luma = CVPixelBufferGetBaseAddressOfPlane(pb!, 0)!.assumingMemoryBound(to: UInt8.self)
            let bpr = CVPixelBufferGetBytesPerRowOfPlane(pb!, 0)
            for y in 0..<360 { memset(luma + y * bpr, Int32((y + i * 4) & 0xFF), 640) }
            let chroma = CVPixelBufferGetBaseAddressOfPlane(pb!, 1)!.assumingMemoryBound(to: UInt8.self)
            for y in 0..<180 { memset(chroma + y * CVPixelBufferGetBytesPerRowOfPlane(pb!, 1), 128, 640) }
            CVPixelBufferUnlockBaseAddress(pb!, [])
            encoder.encode(pixelBuffer: pb!, presentationTime: CMTime(value: Int64(i), timescale: 120),
                           duration: CMTime(value: 1, timescale: 120), captureUs: HostClock.nowUs(),
                           captureIsReal: true)
        }
        wait(for: [done], timeout: 20)
        let load = encoder.loadSecond()
        encoder.stop()
        XCTAssertEqual(load.last.calls, frameCount, "every encode call is timed")
        XCTAssertFalse(load.behind)
        XCTAssertLessThan(load.last.avgCallMs, 1000.0 / 120, "a small frame encodes well inside 8.3 ms")
    }
}
