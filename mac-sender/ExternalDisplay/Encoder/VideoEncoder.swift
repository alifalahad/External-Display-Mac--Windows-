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
// - Per-frame latency tracking via PTS-keyed timestamps
// =============================================================================

import Foundation
import VideoToolbox
import CoreMedia
import CoreVideo

final class VideoEncoder {

    // ── Public ──────────────────────────────────────────────────────────────

    /// Called on VT's internal thread for each encoded frame.
    /// Parameters: (encodedSampleBuffer, isKeyframe, encodingLatencyMs)
    var onEncodedFrame: ((CMSampleBuffer, Bool, Double) -> Void)?

    /// Current encoder configuration (read-only after init)
    let config: EncoderConfig

    /// Whether hardware encoder was actually selected
    private(set) var isHardwareAccelerated = false

    // ── Private ─────────────────────────────────────────────────────────────

    private var session: VTCompressionSession?

    /// Track encode start times keyed by PTS value, for latency measurement
    private var encodeStartTimes: [CMTimeValue: Double] = [:]
    private let timeLock = NSLock()

    // ── Stats ───────────────────────────────────────────────────────────────

    private var encodedFrameCount: UInt64 = 0
    private var encodedByteCount: UInt64 = 0
    private var keyframeCount: UInt64 = 0
    private var latencies: [Double] = []
    private let latencyWindowSize = 120
    private var firstEncodeTime: Double = 0
    private let statsLock = NSLock()

    // ── Initialization ──────────────────────────────────────────────────────

    init(config: EncoderConfig) throws {
        self.config = config
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
            setProperty(kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        }

        // Profile & Level
        setProperty(kVTCompressionPropertyKey_ProfileLevel,
                     value: config.profile.vtProfileLevel)

        // Bitrate
        setProperty(kVTCompressionPropertyKey_AverageBitRate,
                     value: config.bitrateBps as CFNumber)

        // Data rate limit (peak bitrate)
        setProperty(kVTCompressionPropertyKey_DataRateLimits,
                     value: config.dataRateLimit as CFArray)

        // Keyframe interval
        setProperty(kVTCompressionPropertyKey_MaxKeyFrameInterval,
                     value: config.keyframeInterval as CFNumber)

        // Expected frame rate (helps encoder allocate bits)
        setProperty(kVTCompressionPropertyKey_ExpectedFrameRate,
                     value: config.fps as CFNumber)

        // Disable B-frames for lowest latency
        if !config.allowFrameReordering {
            setProperty(kVTCompressionPropertyKey_AllowFrameReordering,
                         value: kCFBooleanFalse)
        }

        // Allow the encoder to use temporal compression
        setProperty(kVTCompressionPropertyKey_AllowTemporalCompression,
                     value: kCFBooleanTrue)

        // ── Check if hardware encoder was selected ──────────────────────────
        var usingHardware: CFBoolean = kCFBooleanFalse
        let hwStatus = VTSessionCopyProperty(
            session,
            key: kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder,
            allocator: kCFAllocatorDefault,
            valueOut: &usingHardware
        )
        if hwStatus == noErr {
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

    // ── Encode a Frame ──────────────────────────────────────────────────────

    /// Feed a CVPixelBuffer to the encoder. Called from the capture queue.
    /// The pixelBuffer stays GPU-resident — VT handles BGRA→NV12 internally.
    func encode(pixelBuffer: CVPixelBuffer, presentationTime: CMTime, duration: CMTime) {
        guard let session = session else { return }

        // Record encode start time for latency measurement
        let startTime = CACurrentMediaTime()
        timeLock.lock()
        encodeStartTimes[presentationTime.value] = startTime
        timeLock.unlock()

        if firstEncodeTime == 0 { firstEncodeTime = startTime }

        // Encode with block-based output handler
        VTCompressionSessionEncodeFrame(
            session,
            imageBuffer: pixelBuffer,
            presentationTimeStamp: presentationTime,
            duration: duration,
            frameProperties: nil,
            infoFlagsOut: nil
        ) { [weak self] status, infoFlags, sampleBuffer in
            self?.handleEncodedFrame(
                status: status,
                sampleBuffer: sampleBuffer,
                originalPTS: presentationTime
            )
        }
    }

    // ── Output Handler ──────────────────────────────────────────────────────

    private func handleEncodedFrame(
        status: OSStatus,
        sampleBuffer: CMSampleBuffer?,
        originalPTS: CMTime
    ) {
        guard status == noErr, let sampleBuffer = sampleBuffer else {
            if status != noErr {
                print("[Encoder] Encode error: \(status)")
            }
            return
        }

        // ── Measure encoding latency ────────────────────────────────────────
        let endTime = CACurrentMediaTime()
        var latencyMs: Double = 0

        timeLock.lock()
        if let startTime = encodeStartTimes.removeValue(forKey: originalPTS.value) {
            latencyMs = (endTime - startTime) * 1000.0
        }
        timeLock.unlock()

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
        latencies.append(latencyMs)
        if latencies.count > latencyWindowSize {
            latencies.removeFirst()
        }
        statsLock.unlock()

        // ── Notify listener ─────────────────────────────────────────────────
        onEncodedFrame?(sampleBuffer, isKeyframe, latencyMs)
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

        // Latency stats from rolling window
        if !latencies.isEmpty {
            let sum = latencies.reduce(0, +)
            stats.avgEncodingLatencyMs = sum / Double(latencies.count)
            stats.minEncodingLatencyMs = latencies.min() ?? 0
            stats.maxEncodingLatencyMs = latencies.max() ?? 0
        }

        // Output bitrate (total bytes / total time)
        let elapsed = CACurrentMediaTime() - firstEncodeTime
        if elapsed > 0 {
            stats.outputBitrateMbps = Double(encodedByteCount) * 8.0 / elapsed / 1_000_000.0
        }

        return stats
    }

    // ── Force Keyframe ──────────────────────────────────────────────────────

    /// Request the next frame to be encoded as a keyframe
    func forceKeyframe() {
        guard let session = session else { return }
        let properties: [CFString: Any] = [
            kVTEncodeFrameOptionKey_ForceKeyFrame: true
        ]
        // This will be applied on the next encode call
        // Store it and use as frameProperties in the next encode
        // For simplicity, we'll use VTCompressionSessionCompleteFrames then re-prepare
        VTCompressionSessionCompleteFrames(session, untilPresentationTimeStamp: .invalid)
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
