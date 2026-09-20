// =============================================================================
// ContentView.swift — Main UI for the Mac sender
// =============================================================================
// Shows a display picker, virtual display controls, start/stop controls,
// live preview of captured frames, and real-time capture statistics.
// =============================================================================

import SwiftUI
import ScreenCaptureKit

struct ContentView: View {
    @StateObject private var engine = ScreenCaptureEngine()

    // Virtual display resolution presets
    private let resolutionPresets: [(String, Int, Int)] = [
        ("1920×1080 (Full HD)", 1920, 1080),
        ("1536×864", 1536, 864),
        ("1366×768", 1366, 768),
        ("2560×1440 (QHD)", 2560, 1440),
        ("2560×1600", 2560, 1600),
        ("1920×1200", 1920, 1200),
    ]
    @State private var selectedResolution = 0

    var body: some View {
        VStack(spacing: 0) {
            // ── Toolbar ─────────────────────────────────────────────────────
            toolbar
                .padding(.horizontal, 16)
                .padding(.vertical, 10)
                .background(.ultraThinMaterial)

            Divider()

            // ── Virtual Display Controls ────────────────────────────────────
            virtualDisplayBar
                .padding(.horizontal, 16)
                .padding(.vertical, 8)
                .background(Color(nsColor: .controlBackgroundColor))

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
        .frame(minWidth: 800, minHeight: 600)
        .task {
            await engine.refreshDisplays()
        }
    }

    // ── Virtual Display Controls ────────────────────────────────────────────

    private var virtualDisplayBar: some View {
        HStack(spacing: 12) {
            Image(systemName: engine.virtualDisplayManager.isActive
                  ? "display.2" : "rectangle.badge.plus")
                .font(.title3)
                .foregroundStyle(engine.virtualDisplayManager.isActive ? .green : .secondary)

            if engine.virtualDisplayManager.isActive {
                // Active state
                VStack(alignment: .leading, spacing: 2) {
                    Text("Virtual Display Active")
                        .font(.caption)
                        .foregroundStyle(.green)
                    Text("ID: \(engine.virtualDisplayManager.virtualDisplayID)  •  \(engine.virtualDisplayManager.width)×\(engine.virtualDisplayManager.height)")
                        .font(.system(.caption2, design: .monospaced))
                        .foregroundStyle(.secondary)
                }

                Spacer()

                Text("Arrange in System Settings → Displays")
                    .font(.caption2)
                    .foregroundStyle(.secondary)

                Button {
                    engine.stopCapture()
                    engine.virtualDisplayManager.destroyDisplay()
                    Task { await engine.refreshDisplays() }
                } label: {
                    Label("Remove", systemImage: "xmark.circle")
                }
                .buttonStyle(.bordered)
                .tint(.red)
                .disabled(engine.isCapturing)

            } else {
                // Inactive state — show creation controls
                Text("Virtual Display")
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Picker("Resolution:", selection: $selectedResolution) {
                    ForEach(0..<resolutionPresets.count, id: \.self) { i in
                        Text(resolutionPresets[i].0).tag(i)
                    }
                }
                .frame(maxWidth: 200)

                Spacer()

                Button {
                    let preset = resolutionPresets[selectedResolution]
                    if engine.virtualDisplayManager.createDisplay(
                        width: preset.1, height: preset.2
                    ) {
                        // Wait a bit for macOS to register the display, then refresh
                        Task {
                            try? await Task.sleep(nanoseconds: 1_000_000_000)
                            await engine.refreshDisplays()
                        }
                    }
                } label: {
                    Label("Create Virtual Display", systemImage: "rectangle.badge.plus")
                }
                .buttonStyle(.borderedProminent)
                .disabled(engine.isCapturing)
            }
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

            // Display picker — show when NOT using virtual display
            if !engine.virtualDisplayManager.isActive {
                if engine.availableDisplays.isEmpty {
                    Text(engine.errorMessage != nil ? "No permission" : "Loading displays…")
                        .foregroundStyle(.secondary)
                        .font(.caption)
                        .padding(.horizontal, 8)
                        .padding(.vertical, 4)
                        .background(.quaternary, in: RoundedRectangle(cornerRadius: 6))
                } else {
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
            } else {
                Text("Capturing: Virtual Display \(engine.virtualDisplayManager.virtualDisplayID)")
                    .font(.caption)
                    .foregroundStyle(.green)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    .background(.green.opacity(0.1), in: RoundedRectangle(cornerRadius: 6))
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

            // Start / Stop — disabled when no displays found
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
            .disabled(!engine.isCapturing && engine.availableDisplays.isEmpty
                       && !engine.virtualDisplayManager.isActive)
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
                Color(nsColor: .windowBackgroundColor)
                    .overlay {
                        VStack(spacing: 16) {
                            if engine.availableDisplays.isEmpty && engine.errorMessage != nil {
                                // Permission denied state
                                Image(systemName: "lock.shield")
                                    .font(.system(size: 48))
                                    .foregroundStyle(.orange)

                                Text("Screen Recording Permission Required")
                                    .font(.headline)

                                Text("Open System Settings → Privacy & Security → Screen Recording\nand enable access for this app, then click ↺ Refresh.")
                                    .font(.callout)
                                    .foregroundStyle(.secondary)
                                    .multilineTextAlignment(.center)
                                    .frame(maxWidth: 420)

                                Button("Open Privacy Settings") {
                                    NSWorkspace.shared.open(
                                        URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture")!
                                    )
                                }
                                .buttonStyle(.borderedProminent)

                            } else if engine.availableDisplays.isEmpty {
                                Image(systemName: "display")
                                    .font(.system(size: 48))
                                    .foregroundStyle(.tertiary)
                                Text("Looking for displays…")
                                    .foregroundStyle(.secondary)

                            } else if engine.isCapturing {
                                ProgressView()
                                Text("Waiting for frames…")
                                    .foregroundStyle(.secondary)

                            } else if engine.virtualDisplayManager.isActive {
                                Image(systemName: "display.2")
                                    .font(.system(size: 48))
                                    .foregroundStyle(.green.opacity(0.6))
                                Text("Virtual display ready — press Start Capture")
                                    .foregroundStyle(.secondary)
                                Text("Tip: Arrange it in System Settings → Displays first")
                                    .font(.caption)
                                    .foregroundStyle(.tertiary)

                            } else {
                                Image(systemName: "display.trianglebadge.exclamationmark")
                                    .font(.system(size: 48))
                                    .foregroundStyle(.tertiary)
                                Text("Create a virtual display or select a display, then press Start Capture")
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
            }
        }
    }

    // ── Stats Bar ───────────────────────────────────────────────────────────

    private var statsBar: some View {
        VStack(spacing: 4) {
            // Row 1: Capture stats
            HStack(spacing: 16) {
                if engine.isCapturing {
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
                        .font(.system(.caption, design: .monospaced))
                }
                Spacer()
            }

            // Row 2: Encoder stats (when encoding)
            if engine.isEncoding {
                HStack(spacing: 16) {
                    Image(systemName: "cpu")
                        .foregroundStyle(engine.encoderStats.isHardwareAccelerated ? .green : .orange)
                        .font(.caption)

                    statItem("H.264",
                        engine.encoderStats.isHardwareAccelerated ? "HW" : "SW")
                    statItem("Enc Latency",
                        String(format: "%.1f ms", engine.encoderStats.avgEncodingLatencyMs))
                    statItem("Bitrate",
                        String(format: "%.1f Mbps", engine.encoderStats.outputBitrateMbps))
                    statItem("Encoded", "\(engine.encoderStats.encodedFrames)")
                    statItem("Keyframes", "\(engine.encoderStats.keyframes)")
                    statItem("Data",
                        formatBytes(engine.encoderStats.encodedBytes))

                    Spacer()
                }
            }

            // Row 3: Network status
            if engine.isCapturing {
                HStack(spacing: 16) {
                    let senderState = engine.sender.state
                    Image(systemName: senderState.isStreaming ? "wifi" : "wifi.slash")
                        .foregroundStyle(senderState.isStreaming ? .green : .secondary)
                        .font(.caption)

                    statItem("Net", senderState.displayText)

                    if senderState.isStreaming {
                        let ns = engine.sender.networkStats
                        statItem("Sent", formatBytes(ns.bytesSent))
                        statItem("Packets", "\(ns.packetsSent)")
                        statItem("Frames", "\(ns.framesSent)")
                    }

                    Spacer()
                }
            }
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

    private func formatBytes(_ bytes: UInt64) -> String {
        if bytes < 1024 { return "\(bytes) B" }
        if bytes < 1024 * 1024 { return String(format: "%.1f KB", Double(bytes) / 1024) }
        return String(format: "%.1f MB", Double(bytes) / 1024 / 1024)
    }
}
