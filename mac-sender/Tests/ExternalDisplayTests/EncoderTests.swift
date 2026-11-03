// VideoToolbox encoder → Annex B: keyframes, timing prefix room (Phase 20)

import XCTest
import CoreVideo
import CoreMedia
import ScreenCaptureKit
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

    // docs/SMOOTHNESS.md, step 4: capture in the encoder's format, full-range BT.709
    func testCaptureSettings() {
        let config = ScreenCaptureEngine.streamConfiguration(width: 1920, height: 1080, fps: 60, capturePointer: false)
        XCTAssertEqual(config.pixelFormat, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, "NV12, full range")
        XCTAssertEqual(config.colorMatrix as String, CGDisplayStream.yCbCrMatrix_ITU_R_709_2 as String)
        XCTAssertEqual(config.colorSpaceName as String, CGColorSpace.sRGB as String)
        XCTAssertEqual(config.queueDepth, 3)
        XCTAssertFalse(config.showsCursor)
        XCTAssertEqual(config.minimumFrameInterval, CMTime(value: 1, timescale: 60))
    }

    func testStreamSaysFullRangeBT709() throws {
        var config = EncoderConfig()
        config.width = 640
        config.height = 360
        let encoder = try VideoEncoder(config: config)
        let done = expectation(description: "frame")
        var format: CMFormatDescription?
        encoder.onEncodedFrame = { sample, _, _ in
            format = CMSampleBufferGetFormatDescription(sample)
            done.fulfill()
        }
        var pool: CVPixelBufferPool?
        let attrs: [CFString: Any] = [kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
                                      kCVPixelBufferWidthKey: 640, kCVPixelBufferHeightKey: 360,
                                      kCVPixelBufferIOSurfacePropertiesKey: [:] as [String: Any]]
        CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool)
        var pb: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool!, &pb)
        encoder.encode(pixelBuffer: pb!, presentationTime: .zero, duration: CMTime(value: 1, timescale: 60),
                       captureUs: HostClock.nowUs(), captureIsReal: true)
        wait(for: [done], timeout: 10)
        encoder.stop()

        // VideoToolbox's own reading of the SPS it wrote (the receiver parses the same fields)
        let fd = try XCTUnwrap(format)
        func ext(_ key: CFString) -> Any? { CMFormatDescriptionGetExtension(fd, extensionKey: key) }
        XCTAssertEqual(ext(kCMFormatDescriptionExtension_FullRangeVideo) as? Bool, true, "0–255, as the receiver's shader expects")
        XCTAssertEqual(ext(kCVImageBufferYCbCrMatrixKey) as? String, kCVImageBufferYCbCrMatrix_ITU_R_709_2 as String)
        XCTAssertEqual(ext(kCVImageBufferColorPrimariesKey) as? String, kCVImageBufferColorPrimaries_ITU_R_709_2 as String)
        XCTAssertEqual(ext(kCVImageBufferTransferFunctionKey) as? String, kCVImageBufferTransferFunction_ITU_R_709_2 as String)
    }
}
