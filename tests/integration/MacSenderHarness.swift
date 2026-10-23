// =============================================================================
// MacSenderHarness.swift — The real Mac sender with synthetic frames
// =============================================================================
// Integration test (run-loopback.sh): real StreamSender, PairingManager,
// SecureChannel and VideoToolbox encoder; frames are drawn in memory instead
// of captured (no Screen Recording permission needed). New pairings are
// approved automatically once Windows confirmed the code (rejected instead
// when EXDP_REJECT=1); the code is written to a file so the receiver test can
// "type" it.
//
//   harness <seconds> <code-file> <pairing-dir>
// =============================================================================

import Foundation
import CoreVideo
import CoreMedia
import Combine

let args = CommandLine.arguments
let duration = Double(args.count > 1 ? args[1] : "20") ?? 20
let codeFile = args.count > 2 ? args[2] : "code.txt"
let pairingDir = URL(fileURLWithPath: args.count > 3 ? args[3] : "pairings")
let width = 1280, height = 720

let rejectPairings = ProcessInfo.processInfo.environment["EXDP_REJECT"] == "1"
let sender = StreamSender()
let pairing = PairingManager(directory: pairingDir)
sender.pairingManager = pairing
pairing.onDecision = { id, approved in sender.resolvePairing(deviceId: id, approved: approved) }

var cancellables = Set<AnyCancellable>()
pairing.$request.receive(on: DispatchQueue.main).sink { request in
    guard let request else { return }
    if request.windowsConfirmed {
        print("[Harness] Windows confirmed the code — " + (rejectPairings ? "rejecting" : "approving"))
        if rejectPairings { pairing.reject() } else { pairing.approve() }
    } else {
        try? request.verificationCode.write(toFile: codeFile, atomically: true, encoding: .utf8)
        print("[Harness] Pairing request from \(request.deviceName); code written")
    }
}.store(in: &cancellables)
sender.$state.receive(on: DispatchQueue.main).sink { st in
    print("[Harness] state: \(st.displayText)")
    if case .connected = st { sender.startStreaming() }
}.store(in: &cancellables)

var config = EncoderConfig()
config.width = Int32(width)
config.height = Int32(height)
let encoder = try! VideoEncoder(config: config)
encoder.onEncodedFrame = { sb, key, timing in
    guard sender.state.isStreaming else { return }
    if let frame = H264Converter.annexBData(from: sb, isKeyframe: key, reservedPrefix: FrameTimingPrefix.size) {
        sender.sendVideoFrame(frameData: frame, isKeyframe: key, timing: timing)
    }
}
sender.onKeyframeRequest = { encoder.forceKeyframe() }
sender.streamWidth = UInt32(width)
sender.streamHeight = UInt32(height)
sender.startListening()

let attrs: [CFString: Any] = [
    kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_32BGRA,
    kCVPixelBufferWidthKey: width, kCVPixelBufferHeightKey: height,
    kCVPixelBufferIOSurfacePropertiesKey: [:] as [String: Any]]
var pool: CVPixelBufferPool?
CVPixelBufferPoolCreate(nil, nil, attrs as CFDictionary, &pool)

let captureQueue = DispatchQueue(label: "capture", qos: .userInteractive)
var frameIndex: Int64 = 0
var rng = SystemRandomNumberGenerator()
let frameTimer = DispatchSource.makeTimerSource(queue: captureQueue)
frameTimer.schedule(deadline: .now(), repeating: .nanoseconds(16_666_667), leeway: .microseconds(500))
frameTimer.setEventHandler {
    var pbOut: CVPixelBuffer?
    CVPixelBufferPoolCreatePixelBuffer(nil, pool!, &pbOut)
    guard let pb = pbOut else { return }
    CVPixelBufferLockBaseAddress(pb, [])
    let base = CVPixelBufferGetBaseAddress(pb)!.assumingMemoryBound(to: UInt8.self)
    let bpr = CVPixelBufferGetBytesPerRow(pb)
    let barX = Int(frameIndex * 8) % (width - 64)
    for y in 0..<height {
        let row = base + y * bpr
        memset(row, Int32(truncatingIfNeeded: (y + Int(frameIndex)) & 0xFF), width * 4)
        memset(row + barX * 4, 0xFF, 64 * 4)
        if y >= 300 && y < 420 {
            for x in stride(from: 400, to: 880, by: 4) { row[x * 4] = UInt8.random(in: 0...255, using: &rng) }
        }
    }
    CVPixelBufferUnlockBaseAddress(pb, [])
    let pts = CMTime(value: frameIndex, timescale: 60)
    frameIndex += 1
    encoder.encode(pixelBuffer: pb, presentationTime: pts, duration: CMTime(value: 1, timescale: 60),
                   captureUs: HostClock.nowUs(), captureIsReal: true)
}
frameTimer.resume()

let metrics = ProcessMetrics()
_ = metrics.sample()
let statTimer = DispatchSource.makeTimerSource(queue: .main)
statTimer.schedule(deadline: .now() + 1, repeating: 1)
statTimer.setEventHandler {
    let es = encoder.currentStats(), u = metrics.sample()
    guard sender.state.isStreaming else { return }
    var p = SenderStatsPayload()
    p.captureFps100 = 6000
    p.encodeFps100 = UInt32(es.encodedFps * 100)
    p.cpuPercent100 = UInt32(u.cpuPercent * 100)
    p.memoryMB = UInt32(u.memoryMB)
    p.encoderKbps = UInt32(es.outputBitrateMbps * 1000)
    p.targetKbps = UInt32(es.targetBitrateMbps * 1000)
    p.flags = es.isHardwareAccelerated ? SenderStatsPayload.flagHardwareEncoder : 0
    sender.sendSenderStats(p)
}
statTimer.resume()

DispatchQueue.main.asyncAfter(deadline: .now() + duration) {
    let ns = sender.statsSnapshot()
    print("[Harness] done: encoded \(encoder.currentStats().encodedFrames), sent \(ns.framesSent) frames, "
          + "receiver RTT \(ns.receiverRttMs.map { String(format: "%.1f", $0) } ?? "-") ms")
    sender.shutdown()
    exit(0)
}
dispatchMain()
