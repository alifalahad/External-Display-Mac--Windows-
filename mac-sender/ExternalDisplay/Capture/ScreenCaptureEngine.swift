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

    /// Virtual display manager — creates a second display on macOS
    let virtualDisplayManager = VirtualDisplayManager()

    /// Pairing manager — handles secure device authentication
    let pairingManager = PairingManager()

    override init() {
        super.init()
        // Wire pairing manager to sender immediately so it's ready
        // before any connections arrive (fixes: PIN not asked)
        sender.pairingManager = pairingManager

        // ContentView only observes the engine; re-publish nested PIN changes
        // so the PIN appears the moment it is generated.
        pairingCancellable = pairingManager.objectWillChange
            .receive(on: DispatchQueue.main)
            .sink { [weak self] _ in self?.objectWillChange.send() }
    }

    private var pairingCancellable: AnyCancellable?

    // ── Internals ───────────────────────────────────────────────────────────

    private var stream: SCStream?
    private var encoder: VideoEncoder?
    private var frameCounter: UInt64 = 0

    // ── Keyframe recovery (capture queue only) ──────────────────────────────
    /// Last complete frame — re-encoded on keyframe requests, because
    /// ScreenCaptureKit delivers nothing while the screen is static.
    private var lastPixelBuffer: CVPixelBuffer?
    private var lastEncodedPTS = CMTime.invalid
    private var lastKeyframeRequest: Double = 0
    private let minKeyframeRequestInterval = 0.1  // seconds

    // ── Capture auto-restart (main thread) ──────────────────────────────────
    private var captureRestartTask: Task<Void, Never>?
    private var captureRestartAttempts = 0

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

    /// Clipboard sync — bidirectional clipboard between Mac and Windows
    private var clipboardSync: ClipboardSync?

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

            // If virtual display is active, set the display origin offset
            // so mouse events land on the virtual display, not the main screen
            if virtualDisplayManager.isActive {
                let bounds = virtualDisplayManager.getDisplayBounds()
                injector.displayOriginX = bounds.origin.x
                injector.displayOriginY = bounds.origin.y
                print("[Engine] Input mapped to virtual display at (\(bounds.origin.x), \(bounds.origin.y))")
            }
            inputInjector = injector

            // Wire up input events from receiver → injector
            sender.onInputEvent = { [weak injector] event in
                injector?.processEvent(event)
            }

            encoder = enc

            // Set up clipboard sync
            let clipboard = ClipboardSync()
            clipboard.onClipboardChanged = { [weak self] data in
                self?.sender.sendClipboardData(data)
            }
            sender.onClipboardData = { [weak clipboard] data in
                DispatchQueue.main.async {
                    clipboard?.receiveRemoteClipboard(data)
                }
            }
            clipboard.start()
            clipboardSync = clipboard

            DispatchQueue.main.async { [weak self] in
                self?.isEncoding = true
            }

            // Phase 15: receiver asks for a keyframe (stream start, packet loss, decoder reset)
            sender.onKeyframeRequest = { [weak self] in
                self?.requestKeyframe()
            }

            // Phase 15: Set up sleep/wake observer
            setupSleepWakeObserver()

            print("[Engine] Encoder started with adaptive quality + input forwarding + clipboard sync")
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
        captureQueue.async { [weak self] in
            // The old frame may have a different size than the next encoder expects
            self?.lastPixelBuffer = nil
            self?.lastEncodedPTS = .invalid
        }
        qualityController = nil
        inputInjector = nil
        clipboardSync?.stop()
        clipboardSync = nil
        sender.onQualityReport = nil
        sender.onInputEvent = nil
        sender.onClipboardData = nil
        sender.onKeyframeRequest = nil
        removeSleepWakeObserver()
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

    @discardableResult
    func startCapture() async -> Bool {
        // ── Determine which display to capture ──────────────────────────────
        let display: SCDisplay

        if virtualDisplayManager.isActive {
            // Prefer the virtual display when it's active
            let vdID = virtualDisplayManager.virtualDisplayID
            if let vd = availableDisplays.first(where: { $0.displayID == vdID }) {
                display = vd
                print("[Capture] Using virtual display ID=\(vdID)")
            } else {
                // Virtual display not in the list yet — refresh and retry
                await refreshDisplays()
                if let vd = availableDisplays.first(where: { $0.displayID == vdID }) {
                    display = vd
                    print("[Capture] Using virtual display ID=\(vdID) (after refresh)")
                } else {
                    await MainActor.run {
                        self.errorMessage = "Virtual display not found in ScreenCaptureKit. Refresh displays."
                    }
                    return false
                }
            }
        } else {
            // No virtual display — use selected physical display
            guard selectedDisplayIndex < availableDisplays.count else {
                await MainActor.run { self.errorMessage = "No display selected" }
                return false
            }
            display = availableDisplays[selectedDisplayIndex]
        }

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

            // Capture at display's point resolution
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

            // After an auto-restart the receiver may still be streaming —
            // resend stream parameters (resolution may have changed)
            sender.restartStreamIfActive()
            await MainActor.run { self.captureRestartAttempts = 0 }

            let modeStr = virtualDisplayManager.isActive ? "Virtual" : "Physical"
            print("[Capture] Started (\(modeStr)): \(display.width)×\(display.height) @ 60 FPS")
            return true

        } catch {
            await MainActor.run {
                self.errorMessage = "Capture failed: \(error.localizedDescription)"
                self.isCapturing = false
            }
            print("[Capture] Error: \(error)")
            return false
        }
    }

    // ── Stop Capture ────────────────────────────────────────────────────────

    func stopCapture() {
        // A pending auto-restart must not bring capture back after the user stopped it
        captureRestartTask?.cancel()
        captureRestartTask = nil
        captureRestartAttempts = 0

        let stream = self.stream
        guard stream != nil || isCapturing else { return }
        self.stream = nil

        // Stop network sender and encoder
        sender.stop()
        stopEncoder()
        senderCancellable = nil

        Task {
            do {
                try await stream?.stopCapture()
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

    /// Stop everything including virtual display
    func stopAll() {
        stopCapture()
        virtualDisplayManager.destroyDisplay()
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

    // ── Keyframe Requests (Phase 15) ────────────────────────────────────────

    /// Force an IDR frame now. The last captured frame is re-encoded so the
    /// receiver recovers even when the screen is static (no new SCK frames).
    func requestKeyframe() {
        captureQueue.async { [weak self] in
            guard let self = self, let encoder = self.encoder else { return }
            let now = CACurrentMediaTime()
            guard now - self.lastKeyframeRequest >= self.minKeyframeRequestInterval else { return }
            self.lastKeyframeRequest = now

            encoder.forceKeyframe()
            if let pixelBuffer = self.lastPixelBuffer {
                let pts = CMClockGetTime(CMClockGetHostTimeClock())
                self.encodeFrame(pixelBuffer, pts: pts)
            }
            print("[Engine] Keyframe forced")
        }
    }

    /// Encode on the capture queue with strictly increasing timestamps
    private func encodeFrame(_ pixelBuffer: CVPixelBuffer, pts: CMTime) {
        if lastEncodedPTS.isValid && CMTimeCompare(pts, lastEncodedPTS) <= 0 { return }
        lastEncodedPTS = pts
        encoder?.encode(pixelBuffer: pixelBuffer, presentationTime: pts,
                        duration: CMTime(value: 1, timescale: 60))
    }

    // ── Capture Auto-Restart (Phase 15) ─────────────────────────────────────

    /// ScreenCaptureKit stopped on its own (display removed, virtual display
    /// gone, system interruption). Restart with backoff; keep the receiver connected.
    @MainActor
    private func handleCaptureStopped(_ failedStream: SCStream, error: Error) {
        guard failedStream === stream else { return }  // stale or user-stopped

        if let scError = error as? SCStreamError, scError.code == .userStopped {
            print("[Capture] Stopped by user from system UI")
            stopCapture()
            errorMessage = "Capture stopped from the system menu"
            return
        }

        stream = nil
        captureRestartAttempts += 1
        let delay = min(pow(2.0, Double(captureRestartAttempts - 1)), 10.0)
        errorMessage = "Capture interrupted — restarting in \(Int(delay))s…"
        print("[Capture] Restart attempt \(captureRestartAttempts) in \(delay)s")

        captureRestartTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            guard let self = self, !Task.isCancelled else { return }
            await self.restartCapture()
        }
    }

    private func restartCapture() async {
        await refreshDisplays()

        // Virtual display vanished (e.g. WindowServer reset) — recreate it at the same size
        let vdm = virtualDisplayManager
        let vdMissing = await MainActor.run {
            vdm.isActive && !self.availableDisplays.contains { $0.displayID == vdm.virtualDisplayID }
        }
        if vdMissing {
            print("[Capture] Virtual display missing — recreating")
            let (w, h) = await MainActor.run { (vdm.width, vdm.height) }
            await MainActor.run {
                vdm.destroyDisplay()
                _ = vdm.createDisplay(width: w, height: h)
            }
            try? await Task.sleep(nanoseconds: 1_000_000_000)  // let WindowServer register it
            await refreshDisplays()
        }

        let restarted = await startCapture()
        if !restarted {
            await MainActor.run {
                guard self.captureRestartTask?.isCancelled == false else { return }
                self.captureRestartAttempts += 1
                let delay = min(pow(2.0, Double(self.captureRestartAttempts - 1)), 10.0)
                print("[Capture] Restart failed — retrying in \(delay)s")
                self.captureRestartTask = Task { [weak self] in
                    try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
                    guard let self = self, !Task.isCancelled else { return }
                    await self.restartCapture()
                }
            }
        }
    }

    // ── Sleep/Wake Recovery (Phase 15) ──────────────────────────────────────

    private var sleepWakeObservers: [NSObjectProtocol] = []

    /// Observe system sleep/wake to handle recovery
    private func setupSleepWakeObserver() {
        let wakeObs = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.didWakeNotification,
            object: nil, queue: .main
        ) { [weak self] _ in
            guard let self = self, self.isCapturing else { return }
            print("[Engine] ⏰ System wake detected — forcing keyframe for stream recovery")

            // Force keyframe so receiver can resume decoding immediately
            self.requestKeyframe()
        }

        let sleepObs = NSWorkspace.shared.notificationCenter.addObserver(
            forName: NSWorkspace.willSleepNotification,
            object: nil, queue: .main
        ) { _ in
            print("[Engine] 💤 System going to sleep")
        }

        sleepWakeObservers = [wakeObs, sleepObs]
    }

    /// Remove sleep/wake observers
    private func removeSleepWakeObserver() {
        for obs in sleepWakeObservers {
            NSWorkspace.shared.notificationCenter.removeObserver(obs)
        }
        sleepWakeObservers.removeAll()
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
        lastPixelBuffer = pixelBuffer
        encodeFrame(pixelBuffer, pts: CMSampleBufferGetPresentationTimeStamp(sampleBuffer))

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
        Task { @MainActor [weak self] in
            self?.handleCaptureStopped(stream, error: error)
        }
    }
}
