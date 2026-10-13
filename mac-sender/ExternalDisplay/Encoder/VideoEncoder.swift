// =============================================================================
// VideoEncoder.swift — VideoToolbox H.264 hardware encoder
// =============================================================================
// Wraps VTCompressionSession for real-time H.264 encoding.
//
// Pipeline: CVPixelBuffer (BGRA, IOSurface) → VT Hardware Encoder → H.264 NALUs
//
// Key design decisions:
// - GPU-to-GPU: CVPixelBuffer stays on GPU, VT does internal BGRA→NV12 conversion
// - No B-frames: allowFrameReordering=false for lowest latency
// - Real-time priority: VT knows this is a live stream, not offline encode
// - Block-based output handler: simpler than C callback + refcon
// - Per-frame timestamps (capture, encode start/end) travel in the handler's
//   closure, so no PTS-keyed lookup table is needed
// =============================================================================

import Foundation
import VideoToolbox
import CoreMedia
import CoreVideo
import QuartzCore

final class VideoEncoder {

    // ── Public ──────────────────────────────────────────────────────────────

    /// Called on VT's internal thread for each encoded frame.
    /// Parameters: (encodedSampleBuffer, isKeyframe, timing)
    var onEncodedFrame: ((CMSampleBuffer, Bool, FrameTiming) -> Void)?

    /// Current encoder configuration (read-only after init)
    let config: EncoderConfig

    /// Whether hardware encoder was actually selected
    private(set) var isHardwareAccelerated = false

    // ── Private ─────────────────────────────────────────────────────────────

    private var session: VTCompressionSession?

    /// Keyframe request + session-reset flags (set from any thread, consumed in encode)
    private let controlLock = NSLock()
    private var forceNextKeyframe = false
    private var sessionNeedsReset = false

    /// Bitrate currently in effect (adaptive quality may change it at runtime)
    private var currentBitrateBps: Int   // guarded by statsLock after init

    // ── Stats ───────────────────────────────────────────────────────────────

    private var encodedFrameCount: UInt64 = 0
    private var encodedByteCount: UInt64 = 0
    private var keyframeCount: UInt64 = 0
    private var latencies = RollingWindow(capacity: 120)   // ~2 s at 60 FPS
    private var outputRate = RateMeter()                   // frames + bytes, last second
    private let statsLock = NSLock()

    // ── Initialization ──────────────────────────────────────────────────────

    init(config: EncoderConfig) throws {
        self.config = config
        self.currentBitrateBps = config.bitrateBps
        try createSession()
    }

    deinit {
        stop()
    }

    // ── Session Creation ────────────────────────────────────────────────────

    private func createSession() throws {
        // Encoder specification — prefer hardware
        var encoderSpec: [CFString: Any] = [:]
        if config.hardwareAccelerated {
            encoderSpec[kVTVideoEncoderSpecification_EnableHardwareAcceleratedVideoEncoder] = true
            encoderSpec[kVTVideoEncoderSpecification_RequireHardwareAcceleratedVideoEncoder] = false
        }

        // Create session with block-based output handler
        var sessionOut: VTCompressionSession?
        let status = VTCompressionSessionCreate(
            allocator: kCFAllocatorDefault,
            width: config.width,
            height: config.height,
            codecType: kCMVideoCodecType_H264,
            encoderSpecification: encoderSpec as CFDictionary,
            imageBufferAttributes: nil,
            compressedDataAllocator: nil,
            outputCallback: nil,       // Using encodeFrame's outputHandler instead
            refcon: nil,
            compressionSessionOut: &sessionOut
        )

        guard status == noErr, let session = sessionOut else {
            throw EncoderError.sessionCreationFailed(status)
        }

        self.session = session

        // ── Configure session properties ────────────────────────────────────

        // Real-time mode
        if config.realtime {
            setProperty(kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue as Any)
        }

        // Profile & Level
        setProperty(kVTCompressionPropertyKey_ProfileLevel,
                     value: config.profile.vtProfileLevel)

        // Bitrate + peak limit
        statsLock.lock()
        let bitrate = currentBitrateBps
        statsLock.unlock()
        applyBitrate(bitrate)

        // Keyframe interval
        setProperty(kVTCompressionPropertyKey_MaxKeyFrameInterval,
                     value: config.keyframeInterval as CFNumber)

        // Expected frame rate (helps encoder allocate bits)
        setProperty(kVTCompressionPropertyKey_ExpectedFrameRate,
                     value: config.fps as CFNumber)

        // Disable B-frames for lowest latency
        if !config.allowFrameReordering {
            setProperty(kVTCompressionPropertyKey_AllowFrameReordering,
                         value: kCFBooleanFalse as Any)
        }

        // Allow the encoder to use temporal compression
        setProperty(kVTCompressionPropertyKey_AllowTemporalCompression,
                     value: kCFBooleanTrue as Any)

        // ── Check if hardware encoder was selected ──────────────────────────
        var usingHardware: CFBoolean?
        let hwStatus = withUnsafeMutablePointer(to: &usingHardware) {
            VTSessionCopyProperty(
                session,
                key: kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
                allocator: kCFAllocatorDefault,
                valueOut: $0
            )
        }
        if hwStatus == noErr, let usingHardware {
            isHardwareAccelerated = CFBooleanGetValue(usingHardware)
        }

        // Prepare to encode
        let prepStatus = VTCompressionSessionPrepareToEncodeFrames(session)
        guard prepStatus == noErr else {
            throw EncoderError.prepareFailed(prepStatus)
        }

        print("[Encoder] Session created: \(config.width)×\(config.height) "
            + "\(config.profile.rawValue) @ \(config.bitrateMbps) Mbps, "
            + "HW: \(isHardwareAccelerated)")
    }

    // ── Runtime Bitrate Update ──────────────────────────────────────────────

    /// Dynamically change the encoder bitrate (VT supports this without restart)
    func updateBitrate(_ newBps: Int) {
        statsLock.lock()
        currentBitrateBps = newBps
        statsLock.unlock()
        applyBitrate(newBps)
        print("[Encoder] Bitrate updated: \(newBps / 1_000_000) Mbps")
    }

    private func applyBitrate(_ bps: Int) {
        // Average bitrate, peak = 2× average over a 1 second window
        setProperty(kVTCompressionPropertyKey_AverageBitRate, value: bps as CFNumber)
        let peakBytesPerSec = Int(Double(bps) * 2.0 / 8.0)
        let dataRateLimit: [CFNumber] = [peakBytesPerSec as CFNumber, 1 as CFNumber]
        setProperty(kVTCompressionPropertyKey_DataRateLimits, value: dataRateLimit as CFArray)
    }

    // ── Encode a Frame ──────────────────────────────────────────────────────

    /// Feed a CVPixelBuffer to the encoder. Called from the capture queue.
    /// The pixelBuffer stays GPU-resident — VT handles BGRA→NV12 internally.
    /// `captureUs` is when the frame was on screen (HostClock); `captureIsReal`
    /// is false when an old frame is re-encoded for a keyframe.
    func encode(pixelBuffer: CVPixelBuffer, presentationTime: CMTime, duration: CMTime,
                captureUs: UInt64, captureIsReal: Bool) {
        controlLock.lock()
        let needsReset = sessionNeedsReset
        sessionNeedsReset = false
        var forceKeyframe = forceNextKeyframe
        forceNextKeyframe = false
        controlLock.unlock()

        if needsReset {
            recreateSession()
            forceKeyframe = true
        }
        guard let session = session else { return }

        let frameProperties: CFDictionary? = forceKeyframe
            ? [kVTEncodeFrameOptionKey_ForceKeyFrame: kCFBooleanTrue] as CFDictionary
            : nil

        var timing = FrameTiming()
        timing.captureUs = captureUs
        timing.captureIsReal = captureIsReal
        timing.encodeStartUs = HostClock.nowUs()

        // Encode with block-based output handler
        let status = VTCompressionSessionEncodeFrame(
            session,
            imageBuffer: pixelBuffer,
            presentationTimeStamp: presentationTime,
            duration: duration,
            frameProperties: frameProperties,
            infoFlagsOut: nil
        ) { [weak self] status, infoFlags, sampleBuffer in
            var finished = timing
            finished.encodeEndUs = HostClock.nowUs()
            self?.handleEncodedFrame(status: status, sampleBuffer: sampleBuffer, timing: finished)
        }
        if status != noErr {
            handleEncodeFailure(status)
        }
    }

    /// Phase 15: the hardware encoder session can be invalidated (sleep/wake,
    /// GPU reset, media server restart). Rebuild it on the next frame.
    private func handleEncodeFailure(_ status: OSStatus) {
        print("[Encoder] Encode error: \(status)")
        if status == kVTInvalidSessionErr {
            controlLock.lock()
            sessionNeedsReset = true
            controlLock.unlock()
        }
    }

    /// Tear down and recreate the compression session (capture queue only)
    private func recreateSession() {
        print("[Encoder] ♻️ Recreating compression session")
        if let old = session {
            VTCompressionSessionInvalidate(old)
            session = nil
        }
        do {
            try createSession()
        } catch {
            print("[Encoder] Session recreation failed: \(error) — retrying on next frame")
            controlLock.lock()
            sessionNeedsReset = true
            controlLock.unlock()
        }
    }

    // ── Output Handler ──────────────────────────────────────────────────────

    private func handleEncodedFrame(
        status: OSStatus,
        sampleBuffer: CMSampleBuffer?,
        timing: FrameTiming
    ) {
        guard status == noErr, let sampleBuffer = sampleBuffer else {
            if status != noErr {
                handleEncodeFailure(status)
            }
            return
        }

        // ── Check if keyframe ───────────────────────────────────────────────
        let isKeyframe: Bool
        if let attachments = CMSampleBufferGetSampleAttachmentsArray(sampleBuffer, createIfNecessary: false) as? [[CFString: Any]],
           let first = attachments.first {
            // A keyframe does NOT depend on other frames
            let notKeyframe = first[kCMSampleAttachmentKey_NotSync] as? Bool ?? false
            isKeyframe = !notKeyframe
        } else {
            isKeyframe = true  // If no attachments, assume keyframe
        }

        // ── Get encoded data size ───────────────────────────────────────────
        let dataSize: Int
        if let dataBuffer = CMSampleBufferGetDataBuffer(sampleBuffer) {
            dataSize = CMBlockBufferGetDataLength(dataBuffer)
        } else {
            dataSize = 0
        }

        // ── Update stats ────────────────────────────────────────────────────
        statsLock.lock()
        encodedFrameCount += 1
        encodedByteCount += UInt64(dataSize)
        if isKeyframe { keyframeCount += 1 }
        latencies.add(timing.encodeLatencyMs)
        outputRate.record(at: CACurrentMediaTime(), bytes: dataSize)
        statsLock.unlock()

        // ── Notify listener ─────────────────────────────────────────────────
        onEncodedFrame?(sampleBuffer, isKeyframe, timing)
    }

    // ── Stats ───────────────────────────────────────────────────────────────

    /// Get a snapshot of current encoder statistics
    func currentStats() -> EncoderStats {
        statsLock.lock()
        defer { statsLock.unlock() }

        var stats = EncoderStats()
        stats.encodedFrames = encodedFrameCount
        stats.encodedBytes = encodedByteCount
        stats.keyframes = keyframeCount
        stats.isHardwareAccelerated = isHardwareAccelerated
        stats.isEncoding = session != nil

        stats.latency = latencies.summary()
        stats.targetBitrateMbps = Double(currentBitrateBps) / 1_000_000

        // Rates over the last second (lifetime averages hide stalls)
        let now = CACurrentMediaTime()
        stats.encodedFps = outputRate.rate(now: now)
        stats.outputBitrateMbps = outputRate.megabitsPerSecond(now: now)

        return stats
    }

    // ── Force Keyframe ──────────────────────────────────────────────────────

    /// Request the next encoded frame to be an IDR keyframe (thread-safe).
    /// (Flushing with CompleteFrames does NOT produce a keyframe.)
    func forceKeyframe() {
        controlLock.lock()
        forceNextKeyframe = true
        controlLock.unlock()
    }

    // ── Flush & Stop ────────────────────────────────────────────────────────

    /// Flush all pending frames
    func flush() {
        guard let session = session else { return }
        VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
    }

    /// Stop the encoder and release the session
    func stop() {
        if let session = session {
            VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
            VTCompressionSessionInvalidate(session)
            self.session = nil
        }
        print("[Encoder] Stopped. Encoded \(encodedFrameCount) frames, "
            + "\(encodedByteCount / 1024) KB total")
    }

    // ── Helpers ─────────────────────────────────────────────────────────────

    private func setProperty(_ key: CFString, value: Any) {
        guard let session = session else { return }
        let status = VTSessionSetProperty(session, key: key, value: value as CFTypeRef)
        if status != noErr {
            print("[Encoder] Warning: Failed to set \(key): \(status)")
        }
    }
}

// ── Error Types ─────────────────────────────────────────────────────────────

enum EncoderError: LocalizedError {
    case sessionCreationFailed(OSStatus)
    case prepareFailed(OSStatus)

    var errorDescription: String? {
        switch self {
        case .sessionCreationFailed(let code):
            return "VTCompressionSession creation failed (status: \(code))"
        case .prepareFailed(let code):
            return "VTCompressionSession prepare failed (status: \(code))"
        }
    }
}
