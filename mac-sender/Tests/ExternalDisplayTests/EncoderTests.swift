// VideoToolbox encoder → Annex B: keyframes, timing prefix room (Phase 20)

import XCTest
import CoreVideo
import CoreMedia
@testable import ExternalDisplay

final class EncoderTests: XCTestCase {

    func testEncodesForcedKeyframeWithParameterSets() throws {
        var config = EncoderConfig()
        config.width = 640
        config.height = 360
        let encoder = try VideoEncoder(config: config)
        XCTAssertTrue(encoder.isHardwareAccelerated)

        let done = expectation(description: "frames")
        done.expectedFulfillmentCount = 5
        var outputs: [(Data, Bool, FrameTiming)] = []
        let lock = NSLock()
        encoder.onEncodedFrame = { sample, key, timing in
            let annexB = H264Converter.annexBData(from: sample, isKeyframe: key,
                                                  reservedPrefix: FrameTimingPrefix.size)!
            lock.lock(); outputs.append((annexB, key, timing)); lock.unlock()
            done.fulfill()
        }

        var pool: CVPixelBufferPool?
        let attrs: [CFString: Any] = [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
                                      kCVPixelBufferWidthKey: 640, kCVPixelBufferHeightKey: 360,
                                      kCVPixelBufferIOSurfacePropertiesKey: [:] as [String: Any]]
        CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool)
        for i in 0..<5 {
            if i == 3 { encoder.forceKeyframe() }
            var pb: CVPixelBuffer?
            CVPixelBufferPoolCreatePixelBuffer(nil, pool!, &pb)
            encoder.encode(pixelBuffer: pb!, presentationTime: CMTime(value: Int64(i), timescale: 60),
                           duration: CMTime(value: 1, timescale: 60), captureUs: HostClock.nowUs(),
                           captureIsReal: true)
        }
        wait(for: [done], timeout: 10)
        encoder.stop()

        lock.lock(); defer { lock.unlock() }
        XCTAssertTrue(outputs[0].1, "first frame is a keyframe")
        XCTAssertTrue(outputs[3].1, "forced keyframe honoured")
        for (data, key, timing) in outputs {
            XCTAssertEqual(data.prefix(FrameTimingPrefix.size), Data(count: FrameTimingPrefix.size),
                           "room for the timing prefix")
            let bitstream = data.dropFirst(FrameTimingPrefix.size)
            XCTAssertEqual(Array(bitstream.prefix(4)), [0, 0, 0, 1], "Annex B start code")
            if key {
                XCTAssertEqual(bitstream[bitstream.startIndex + 4] & 0x1F, 7, "keyframes start with the SPS")
            }
            XCTAssertGreaterThanOrEqual(timing.encodeEndUs, timing.encodeStartUs)
        }
    }

    // docs/SMOOTHNESS.md, step 2: no keyframe every second, only when asked
    func testKeyframesOnlyWhenAsked() throws {
        var config = EncoderConfig()
        config.width = 640
        config.height = 360
        let encoder = try VideoEncoder(config: config)

        let frameCount = 150   // 2.5 s at 60 FPS: the old 60-frame interval gave keyframes at 0, 60, 120
        let done = expectation(description: "frames")
        done.expectedFulfillmentCount = frameCount
        var keyframes: [Int] = []
        var index = 0
        let lock = NSLock()
        encoder.onEncodedFrame = { _, key, _ in
            lock.lock()
            if key { keyframes.append(index) }
            index += 1
            lock.unlock()
            done.fulfill()
        }

        var pool: CVPixelBufferPool?
        let attrs: [CFString: Any] = [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
                                      kCVPixelBufferWidthKey: 640, kCVPixelBufferHeightKey: 360,
                                      kCVPixelBufferIOSurfacePropertiesKey: [:] as [String: Any]]
        CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool)
        for i in 0..<frameCount {
            if i == 100 { encoder.forceKeyframe() }   // what a KEYFRAME_REQ does
            var pb: CVPixelBuffer?
            CVPixelBufferPoolCreatePixelBuffer(nil, pool!, &pb)
            // A moving bar, so every frame has something to encode
            CVPixelBufferLockBaseAddress(pb!, [])
            let base = CVPixelBufferGetBaseAddress(pb!)!.assumingMemoryBound(to: UInt8.self)
            let bpr = CVPixelBufferGetBytesPerRow(pb!)
            for y in 0..<360 {
                memset(base + y * bpr, 40, 640 * 4)
                memset(base + y * bpr + (i * 4 % 600) * 4, 0xFF, 40 * 4)
            }
            CVPixelBufferUnlockBaseAddress(pb!, [])
            encoder.encode(pixelBuffer: pb!, presentationTime: CMTime(value: Int64(i), timescale: 60),
                           duration: CMTime(value: 1, timescale: 60), captureUs: HostClock.nowUs(),
                           captureIsReal: true)
        }
        wait(for: [done], timeout: 20)
        encoder.stop()

        lock.lock(); defer { lock.unlock() }
        XCTAssertEqual(keyframes, [0, 100], "the first frame and the requested one only")
    }
}
