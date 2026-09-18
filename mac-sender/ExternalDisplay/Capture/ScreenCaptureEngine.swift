// =============================================================================
// ScreenCaptureEngine.swift — ScreenCaptureKit capture engine + H.264 encoder
// =============================================================================
// Captures a selected display at up to 60 FPS via ScreenCaptureKit, then
// feeds CVPixelBuffer frames to:
//   1. VideoToolbox H.264 hardware encoder (GPU→GPU, all frames)
//   2. CIContext → CGImage preview (every 2nd frame, for UI)
//
// Key design decisions:
// - CVPixelBuffer stays GPU-resident — VT handles BGRA→NV12 internally
// - Preview is throttled to ~30 FPS to reduce CPU load
// - Encoder runs at full capture rate for smooth output
// =============================================================================

import Foundation
import ScreenCaptureKit
import CoreMedia
import CoreImage
import VideoToolbox
import Combine

final class ScreenCaptureEngine: NSObject, ObservableObject {

    // ── Published state (read on main thread) ───────────────────────────────

    @Published var capturedFrame: CGImage?
    @Published var stats = CaptureStats()
    @Published var encoderStats = EncoderStats()
    @Published var isCapturing = false
    @Published var isEncoding = false
    @Published var availableDisplays: [SCDisplay] = []
    @Published var selectedDisplayIndex: Int = 0
    @Published var errorMessage: String?

    /// Network sender — published so ContentView can observe connection state
    let sender = StreamSender()

    // ── Internals ───────────────────────────────────────────────────────────

    private var stream: SCStream?
    private var encoder: VideoEncoder?
    private var frameCounter: UInt64 = 0

    /// Dedicated queue for frame processing — high priority, serial
    private let captureQueue = DispatchQueue(
        label: "com.externaldisplay.capture",
        qos: .userInteractive
    )

    /// CIContext for CVPixelBuffer → CGImage conversion (GPU-accelerated)
    private let ciContext = CIContext(options: [
        .useSoftwareRenderer: false,
        .cacheIntermediates: false       // Don't cache — each frame is unique
    ])

    private let statsTracker = CaptureStatsTracker()

    /// Adaptive quality controller — adjusts bitrate based on receiver feedback
    private var qualityController: AdaptiveQualityController?

    /// Input injector — forwards mouse/keyboard from Windows to Mac
    private var inputInjector: InputInjector?

    // ── Encoder Control ─────────────────────────────────────────────────────

    /// Start the H.264 encoder with the given resolution
    func startEncoder(width: Int, height: Int) {
        stopEncoder()
        do {
            var config = EncoderConfig()
            config.width = Int32(width)
            config.height = Int32(height)
            let enc = try VideoEncoder(config: config)

            enc.onEncodedFrame = { [weak self] sampleBuffer, isKeyframe, latencyMs in
                // Convert AVCC → Annex B and send over network
                guard let self = self, self.sender.state.isStreaming else { return }
                if let annexB = H264Converter.annexBData(from: sampleBuffer, isKeyframe: isKeyframe) {
                    self.sender.sendVideoFrame(annexBData: annexB, isKeyframe: isKeyframe)
                }
            }

            // Configure sender with stream parameters
            sender.streamWidth = UInt32(width)
            sender.streamHeight = UInt32(height)

            // Set up adaptive quality controller
            let controller = AdaptiveQualityController()
            controller.onBitrateChange = { [weak enc] newBps in
                enc?.updateBitrate(newBps)
            }
            qualityController = controller

            // Wire up quality reports from receiver → controller
            sender.onQualityReport = { [weak controller] report in
                controller?.processReport(
                    packetLossPercent100: report.packetLossPercent,
                    rttMs: report.rttMs,
                    framesDropped: report.framesDropped,
                    queueDepth: report.queueDepth
                )
            }

            // Set up input injector
            let injector = InputInjector()
            injector.displayWidth = CGFloat(width)
            injector.displayHeight = CGFloat(height)
            inputInjector = injector

            // Wire up input events from receiver → injector
            sender.onInputEvent = { [weak injector] event in
                injector?.processEvent(event)
            }

            encoder = enc
            DispatchQueue.main.async { [weak self] in
                self?.isEncoding = true
            }
            print("[Engine] Encoder started with adaptive quality + input forwarding")
        } catch {
            DispatchQueue.main.async { [weak self] in
                self?.errorMessage = "Encoder error: \(error.localizedDescription)"
            }
            print("[Engine] Encoder error: \(error)")
        }
    }

    /// Stop the H.264 encoder
    func stopEncoder() {
        encoder?.stop()
        encoder = nil
        qualityController = nil
        inputInjector = nil
        sender.onQualityReport = nil
        sender.onInputEvent = nil
        DispatchQueue.main.async { [weak self] in
            self?.isEncoding = false
            self?.encoderStats = EncoderStats()
        }
    }

    // ── Display Discovery ───────────────────────────────────────────────────

    /// Fetch available displays from ScreenCaptureKit.
    /// On first call, this triggers the screen recording permission prompt.
    /// Auto-retries every 2 seconds if permission is not yet granted.
    func refreshDisplays() async {
        do {
            let content = try await SCShareableContent.excludingDesktopWindows(
                false, onScreenWindowsOnly: true
            )
            await MainActor.run {
                self.availableDisplays = content.displays
                if self.selectedDisplayIndex >= content.displays.count {
                    self.selectedDisplayIndex = 0
                }
                self.errorMessage = nil
            }
        } catch {
            await MainActor.run {
                // Keep availableDisplays empty so UI shows permission state
                self.availableDisplays = []
                // Store the error but simplify the message — full detail in logs
                self.errorMessage = "Permission denied"
            }
            print("[Capture] Permission error: \(error.localizedDescription)")

            // Auto-retry after 2s so user doesn't need to click Refresh
            // manually after granting permission in System Settings
            guard !isCapturing else { return }
            try? await Task.sleep(nanoseconds: 2_000_000_000)
            guard !isCapturing else { return }
            await refreshDisplays()
        }
    }

    // ── Start Capture ───────────────────────────────────────────────────────

    func startCapture() async {
        guard selectedDisplayIndex < availableDisplays.count else {
            await MainActor.run { self.errorMessage = "No display selected" }
            return
        }

        let display = availableDisplays[selectedDisplayIndex]

        do {
            // Re-fetch content to get current windows for exclusion
            let content = try await SCShareableContent.excludingDesktopWindows(
                false, onScreenWindowsOnly: true
            )

            // Exclude our own app's windows to avoid the infinite mirror effect
            let excludedWindows = content.windows.filter {
                $0.owningApplication?.bundleIdentifier == Bundle.main.bundleIdentifier
            }

            // Configure the content filter
            let filter = SCContentFilter(
                display: display,
                excludingWindows: excludedWindows
            )

            // Configure the stream
            let config = SCStreamConfiguration()

            // Capture at display's point resolution (1x, not Retina 2x)
            // This keeps data volume manageable for Phase 2
            config.width = display.width
            config.height = display.height

            // 60 FPS target
            config.minimumFrameInterval = CMTime(value: 1, timescale: 60)

            // BGRA pixel format — standard for display capture
            config.pixelFormat = kCVPixelFormatType_32BGRA

            // Cursor visibility
            config.showsCursor = true

            // Bounded queue — if we can't keep up, older frames are dropped
            config.queueDepth = 3

            // Create and configure the stream
            let stream = SCStream(filter: filter, configuration: config, delegate: self)

            try stream.addStreamOutput(
                self,
                type: .screen,
                sampleHandlerQueue: captureQueue
            )

            try await stream.startCapture()

            self.stream = stream
            statsTracker.reset()
            frameCounter = 0

            await MainActor.run {
                self.isCapturing = true
                self.errorMessage = nil
            }

            // Auto-start encoder with capture resolution
            startEncoder(width: display.width, height: display.height)

            // Start network listener (Windows receiver will connect here)
            sender.startListening()

            // Watch for connection state changes — auto-start streaming
            setupSenderObserver()

            print("[Capture] Started: \(display.width)×\(display.height) @ 60 FPS")

        } catch {
            await MainActor.run {
                self.errorMessage = "Capture failed: \(error.localizedDescription)"
                self.isCapturing = false
            }
            print("[Capture] Error: \(error)")
        }
    }

    // ── Stop Capture ────────────────────────────────────────────────────────

    func stopCapture() {
        guard let stream = stream else { return }
        self.stream = nil

        // Stop network sender and encoder
        sender.stop()
        stopEncoder()
        senderCancellable = nil

        Task {
            do {
                try await stream.stopCapture()
            } catch {
                print("[Capture] Stop error: \(error)")
            }
            await MainActor.run {
                self.isCapturing = false
                self.capturedFrame = nil
            }
            print("[Capture] Stopped. Total frames: \(statsTracker.totalFrames)")
        }
    }

    // ── Sender State Observer ───────────────────────────────────────────────

    private var senderCancellable: AnyCancellable?

    /// Watch sender state — auto-start streaming when a peer connects
    private func setupSenderObserver() {
        senderCancellable = sender.$state
            .receive(on: DispatchQueue.main)
            .sink { [weak self] newState in
                if case .connected = newState {
                    // Peer connected — begin streaming
                    self?.sender.startStreaming()
                }
            }
    }
}

// MARK: - SCStreamOutput (Frame Processing)

extension ScreenCaptureEngine: SCStreamOutput {

    func stream(
        _ stream: SCStream,
        didOutputSampleBuffer sampleBuffer: CMSampleBuffer,
        of type: SCStreamOutputType
    ) {
        // Only process screen frames (not audio)
        guard type == .screen else { return }

        // ── Check frame status ──────────────────────────────────────────────
        guard let attachmentsArray = CMSampleBufferGetSampleAttachmentsArray(
                  sampleBuffer, createIfNecessary: false
              ) as? [[String: Any]],
              let attachments = attachmentsArray.first,
              let statusRawValue = attachments[SCStreamFrameInfo.status.rawValue] as? Int,
              let status = SCFrameStatus(rawValue: statusRawValue)
        else {
            return
        }

        // Only process complete frames
        switch status {
        case .complete:
            break // Process below
        case .idle:
            return // Screen unchanged — no new content
        case .blank, .suspended, .stopped:
            return
        case .started:
            print("[Capture] Stream started")
            return
        @unknown default:
            return
        }

        // ── Extract CVPixelBuffer ───────────────────────────────────────────
        guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else {
            return
        }

        let width = CVPixelBufferGetWidth(pixelBuffer)
        let height = CVPixelBufferGetHeight(pixelBuffer)

        // ── Record timing ───────────────────────────────────────────────────
        let captureTime = CACurrentMediaTime()
        statsTracker.recordFrame(time: captureTime, width: width, height: height)
        frameCounter += 1

        // ── Feed ALL frames to encoder (GPU→GPU, fast) ──────────────────────
        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        let duration = CMTime(value: 1, timescale: 60)
        encoder?.encode(pixelBuffer: pixelBuffer, presentationTime: pts, duration: duration)

        // ── Throttled preview (every 2nd frame = ~30 FPS) ───────────────────
        // CGImage conversion is expensive (GPU→CPU). Encoder gets all frames;
        // preview can afford to skip some for better overall performance.
        let currentStats = statsTracker.currentStats()
        let encStats = encoder?.currentStats()

        if frameCounter % 2 == 0 {
            let ciImage = CIImage(cvPixelBuffer: pixelBuffer)
            guard let cgImage = ciContext.createCGImage(ciImage, from: ciImage.extent) else {
                return
            }
            DispatchQueue.main.async { [weak self] in
                self?.capturedFrame = cgImage
                self?.stats = currentStats
                if let encStats { self?.encoderStats = encStats }
            }
        } else {
            // Still update stats on non-preview frames
            DispatchQueue.main.async { [weak self] in
                self?.stats = currentStats
                if let encStats { self?.encoderStats = encStats }
            }
        }
    }
}

// MARK: - SCStreamDelegate (Error Handling)

extension ScreenCaptureEngine: SCStreamDelegate {

    func stream(_ stream: SCStream, didStopWithError error: Error) {
        print("[Capture] Stream stopped with error: \(error)")
        DispatchQueue.main.async { [weak self] in
            self?.isCapturing = false
            self?.errorMessage = "Capture stopped: \(error.localizedDescription)"
        }
    }
}
