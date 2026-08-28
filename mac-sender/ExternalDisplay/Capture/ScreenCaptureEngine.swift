// =============================================================================
// ScreenCaptureEngine.swift — ScreenCaptureKit capture engine
// =============================================================================
// Wraps ScreenCaptureKit to capture a selected display at up to 60 FPS.
// Produces CVPixelBuffer frames (GPU-backed via IOSurface) and converts
// them to CGImage for local preview. Stats are tracked per-frame.
//
// Key design decisions:
// - CVPixelBuffer stays GPU-resident (IOSurface-backed) from ScreenCaptureKit
// - CIContext.createCGImage is used for preview (involves GPU→CPU copy,
//   acceptable for Phase 2; will be replaced by Metal in later phases)
// - Frame processing runs on a dedicated high-priority queue
// - UI updates dispatched to main thread
// =============================================================================

import Foundation
import ScreenCaptureKit
import CoreMedia
import CoreImage
import Combine

final class ScreenCaptureEngine: NSObject, ObservableObject {

    // ── Published state (read on main thread) ───────────────────────────────

    @Published var capturedFrame: CGImage?
    @Published var stats = CaptureStats()
    @Published var isCapturing = false
    @Published var availableDisplays: [SCDisplay] = []
    @Published var selectedDisplayIndex: Int = 0
    @Published var errorMessage: String?

    // ── Internals ───────────────────────────────────────────────────────────

    private var stream: SCStream?

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

    // ── Display Discovery ───────────────────────────────────────────────────

    /// Fetch available displays from ScreenCaptureKit.
    /// On first call, this triggers the screen recording permission prompt.
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
                self.errorMessage = """
                    Screen recording permission required.
                    Go to System Settings → Privacy & Security → Screen Recording
                    and grant access to this app (or Xcode/Terminal).
                    
                    Error: \(error.localizedDescription)
                    """
            }
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

            await MainActor.run {
                self.isCapturing = true
                self.errorMessage = nil
            }

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

        // ── Preserve timestamp ──────────────────────────────────────────────
        // The presentation timestamp from ScreenCaptureKit — will be used in
        // Phase 3+ for encoder synchronization
        let _ = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)

        // ── Convert to CGImage for preview ──────────────────────────────────
        // GPU → CPU path via CIContext. Acceptable for Phase 2 local preview.
        // In Phase 3+, the CVPixelBuffer will go directly to VideoToolbox
        // encoder without this conversion.
        let ciImage = CIImage(cvPixelBuffer: pixelBuffer)
        guard let cgImage = ciContext.createCGImage(ciImage, from: ciImage.extent) else {
            return
        }

        // ── Update UI ───────────────────────────────────────────────────────
        let currentStats = statsTracker.currentStats()
        DispatchQueue.main.async { [weak self] in
            self?.capturedFrame = cgImage
            self?.stats = currentStats
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
