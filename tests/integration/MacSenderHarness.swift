// =============================================================================
// MacSenderHarness.swift — The real Mac sender with synthetic frames
// =============================================================================
// Integration test (run-loopback.sh): real StreamSender, PairingManager,
// SecureChannel and VideoToolbox encoder; frames are drawn in memory instead
// of captured (no Screen Recording permission needed). New pairings are
// approved automatically once Windows confirmed the code (rejected instead
// when EXDP_REJECT=1); the code is written to a file so the receiver test can
// "type" it. When the receiver asks to draw the pointer (RECEIVER_CAPS), the
// real PointerMonitor runs for a second, then a scripted pointer path follows
// that the receiver test checks exactly.
//
//   harness <seconds> <code-file> <pairing-dir>
// =============================================================================

import Foundation
import AppKit
import CoreVideo
import CoreMedia
import Combine

_ = NSApplication.shared   // NSCursor needs AppKit set up
setvbuf(stdout, nil, _IOLBF, 0)   // lines reach the log at once (also if the run is killed)

let args = CommandLine.arguments
let duration = Double(args.count > 1 ? args[1] : "20") ?? 20
let codeFile = args.count > 2 ? args[2] : "code.txt"
let pairingDir = URL(fileURLWithPath: args.count > 3 ? args[3] : "pairings")
let width = 1280, height = 720

let rejectPairings = ProcessInfo.processInfo.environment["EXDP_REJECT"] == "1"
let sender = StreamSender()
let governor = BitrateGovernor(preset: .balanced)
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
    if case .connected = st { governor.reset(); sender.startStreaming() }
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

// Adaptive bitrate exactly as in the app (docs/SMOOTHNESS.md, step 6)
governor.onBitrateChange = { encoder.updateBitrate($0) }
governor.encoderOutputBps = { Int(encoder.currentStats().outputBitrateMbps * 1_000_000) }
governor.sentBps = { Int(sender.statsSnapshot().sendMbps * 1_000_000) }
sender.onQualityReport = { governor.onQualityReport($0) }
sender.onRateFeedback = { governor.onRateFeedback($0) }

// Pointer drawn by the receiver (docs/SMOOTHNESS.md, step 1)
var pointerMonitor: PointerMonitor?
var pointerScript: DispatchSourceTimer?
sender.onReceiverCaps = { flags in
    DispatchQueue.main.async {
        print("[Harness] RECEIVER_CAPS 0x\(String(flags, radix: 16))")
        pointerMonitor?.stop()
        pointerMonitor = nil
        pointerScript?.cancel()
        pointerScript = nil
        guard flags & ReceiverCapsPayload.localCursor != 0 else { return }

        // The real monitor: this Mac's pointer image and position
        let monitor = PointerMonitor(displayID: CGMainDisplayID(), streamWidth: width, echo: PointerEcho())
        monitor.onShape = { sender.sendCursorShape($0) }
        monitor.onPosition = { sender.sendCursorPosition($0) }
        monitor.start()
        pointerMonitor = monitor

        // Then a path the receiver can check: the arrow, 100 steps along the
        // diagonal at 120 Hz, then off to another display
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) {
            guard pointerMonitor === monitor else { return }
            monitor.stop()
            pointerMonitor = nil
            if let arrow = PointerImage.render(.arrow, streamPixelsPerPoint: 1) { sender.sendCursorShape(arrow) }
            var step = 0
            let script = DispatchSource.makeTimerSource(queue: .main)
            script.schedule(deadline: .now(), repeating: .microseconds(8333))
            script.setEventHandler {
                if step < 100 {
                    let t = Float(step) / 99
                    sender.sendCursorPosition(CursorPositionPayload(x: t, y: t, flags: CursorPositionPayload.visible))
                } else {
                    sender.sendCursorPosition(CursorPositionPayload(x: 0, y: 0, flags: 0))
                    script.cancel()
                    print("[Harness] pointer: scripted path sent")
                }
                step += 1
            }
            script.resume()
            pointerScript = script
        }
    }
}
sender.streamWidth = UInt32(width)
sender.streamHeight = UInt32(height)
sender.startListening()

// Full-range NV12, as the app captures since docs/SMOOTHNESS.md step 4
let attrs: [CFString: Any] = [
    kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
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
    let luma = CVPixelBufferGetBaseAddressOfPlane(pb, 0)!.assumingMemoryBound(to: UInt8.self)
    let lumaBpr = CVPixelBufferGetBytesPerRowOfPlane(pb, 0)
    let chroma = CVPixelBufferGetBaseAddressOfPlane(pb, 1)!.assumingMemoryBound(to: UInt8.self)
    let chromaBpr = CVPixelBufferGetBytesPerRowOfPlane(pb, 1)
    let barX = Int(frameIndex * 8) % (width - 64)
    for y in 0..<height {
        let row = luma + y * lumaBpr
        memset(row, Int32(truncatingIfNeeded: (y + Int(frameIndex)) & 0xFF), width)
        memset(row + barX, 0xFF, 64)
        if y >= 300 && y < 420 {
            for x in stride(from: 400, to: 880, by: 4) { row[x] = UInt8.random(in: 0...255, using: &rng) }
        }
    }
    for y in 0..<height / 2 { memset(chroma + y * chromaBpr, 128, width) }   // grey: no colour
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
    p.pacingSpreadUs = UInt32(sender.statsSnapshot().pacingSpreadUs)
    sender.sendSenderStats(p)
}
statTimer.resume()

// Every 2 s: frames encoded and sent, state, and whether the network queue answers
let startedAt = CACurrentMediaTime()
let heartbeat = DispatchSource.makeTimerSource(queue: DispatchQueue.global(qos: .utility))
heartbeat.schedule(deadline: .now() + 2, repeating: 2)
heartbeat.setEventHandler {
    let answered = DispatchSemaphore(value: 0)
    sender.probeNetworkQueue { answered.signal() }
    let alive = answered.wait(timeout: .now() + 0.5) == .success
    print(String(format: "[Harness] t=%.0fs encoded %llu sent %llu, %@, network queue %@",
                 CACurrentMediaTime() - startedAt, encoder.currentStats().encodedFrames,
                 sender.statsSnapshot().framesSent, sender.state.displayText, alive ? "ok" : "NOT ANSWERING"))
}
heartbeat.resume()

DispatchQueue.main.asyncAfter(deadline: .now() + duration) {
    let ns = sender.statsSnapshot()
    print("[Harness] done: encoded \(encoder.currentStats().encodedFrames), sent \(ns.framesSent) frames, "
          + "receiver RTT \(ns.receiverRttMs.map { String(format: "%.1f", $0) } ?? "-") ms")
    sender.shutdown()
    exit(0)
}
dispatchMain()
