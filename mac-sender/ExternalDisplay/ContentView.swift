// =============================================================================
// ContentView.swift — Main UI for the Mac sender
// =============================================================================
// Shows a display picker, start/stop controls, live preview of captured
// frames, and real-time capture statistics.
// =============================================================================

import SwiftUI
import ScreenCaptureKit

struct ContentView: View {
    @StateObject private var engine = ScreenCaptureEngine()

    var body: some View {
        VStack(spacing: 0) {
            // ── Toolbar ─────────────────────────────────────────────────────
            toolbar
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .background(.ultraThinMaterial)

            Divider()

            // ── Preview ─────────────────────────────────────────────────────
            preview
                .frame(maxWidth: .infinity, maxHeight: .infinity)

            Divider()

            // ── Stats Bar ───────────────────────────────────────────────────
            statsBar
                .padding(.horizontal, 16)
                .padding(.vertical, 8)
                .background(.ultraThinMaterial)
        }
        .frame(minWidth: 800, minHeight: 550)
        .task {
            await engine.refreshDisplays()
        }
    }

    // ── Toolbar ─────────────────────────────────────────────────────────────

    private var toolbar: some View {
        HStack(spacing: 12) {
            // App icon
            Image(systemName: "display")
                .font(.title2)
                .foregroundStyle(.secondary)

            Text("External Display")
                .font(.headline)

            Spacer()

            // Display picker
            if !engine.availableDisplays.isEmpty {
                Picker("Display:", selection: $engine.selectedDisplayIndex) {
                    ForEach(
                        Array(engine.availableDisplays.enumerated()),
                        id: \.offset
                    ) { index, display in
                        Text("Display \(display.displayID) — \(display.width)×\(display.height)")
                            .tag(index)
                    }
                }
                .frame(maxWidth: 280)
                .disabled(engine.isCapturing)
            }

            // Refresh displays
            Button {
                Task { await engine.refreshDisplays() }
            } label: {
                Image(systemName: "arrow.clockwise")
            }
            .disabled(engine.isCapturing)
            .help("Refresh available displays")

            Divider()
                .frame(height: 20)

            // Start / Stop
            Button {
                Task {
                    if engine.isCapturing {
                        engine.stopCapture()
                    } else {
                        await engine.startCapture()
                    }
                }
            } label: {
                Label(
                    engine.isCapturing ? "Stop Capture" : "Start Capture",
                    systemImage: engine.isCapturing ? "stop.circle.fill" : "play.circle.fill"
                )
            }
            .buttonStyle(.borderedProminent)
            .tint(engine.isCapturing ? .red : .accentColor)
        }
    }

    // ── Preview ─────────────────────────────────────────────────────────────

    private var preview: some View {
        GeometryReader { geometry in
            if let frame = engine.capturedFrame {
                Image(decorative: frame, scale: 1.0)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .frame(
                        width: geometry.size.width,
                        height: geometry.size.height
                    )
            } else {
                // Placeholder
                Color(nsColor: .windowBackgroundColor)
                    .overlay {
                        VStack(spacing: 12) {
                            Image(systemName: "display.trianglebadge.exclamationmark")
                                .font(.system(size: 48))
                                .foregroundStyle(.tertiary)

                            if let error = engine.errorMessage {
                                Text(error)
                                    .font(.caption)
                                    .foregroundStyle(.red)
                                    .multilineTextAlignment(.center)
                                    .frame(maxWidth: 400)
                            } else if engine.isCapturing {
                                Text("Waiting for frames…")
                                    .foregroundStyle(.secondary)
                            } else {
                                Text("Select a display and press Start Capture")
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
            }
        }
    }

    // ── Stats Bar ───────────────────────────────────────────────────────────

    private var statsBar: some View {
        HStack(spacing: 20) {
            if engine.isCapturing {
                // Capture indicator
                Circle()
                    .fill(.red)
                    .frame(width: 8, height: 8)

                statItem("FPS", String(format: "%.1f", engine.stats.fps))
                statItem("Frame", String(format: "%.2f ms", engine.stats.avgFrameTimeMs))
                statItem("Min/Max",
                    String(format: "%.1f / %.1f ms",
                           engine.stats.minFrameTimeMs,
                           engine.stats.maxFrameTimeMs))
                statItem("Resolution",
                    "\(Int(engine.stats.resolution.width))×\(Int(engine.stats.resolution.height))")
                statItem("Frames", "\(engine.stats.totalFrames)")
                statItem("Uptime", String(format: "%.1fs", engine.stats.uptime))
            } else {
                Text("Idle")
                    .foregroundStyle(.secondary)
                    .font(.system(.body, design: .monospaced))
            }

            Spacer()
        }
    }

    private func statItem(_ label: String, _ value: String) -> some View {
        HStack(spacing: 4) {
            Text("\(label):")
                .foregroundStyle(.secondary)
                .font(.system(.caption, design: .monospaced))
            Text(value)
                .font(.system(.caption, design: .monospaced))
        }
    }
}
