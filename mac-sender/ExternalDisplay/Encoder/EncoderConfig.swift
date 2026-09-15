// =============================================================================
// EncoderConfig.swift — H.264 encoder configuration
// =============================================================================
// All configurable parameters for the VideoToolbox H.264 hardware encoder.
// Defaults are tuned for low-latency real-time streaming.
// =============================================================================

import Foundation
import VideoToolbox

/// H.264 profile selection
enum H264Profile: String, CaseIterable, Identifiable {
    case baseline = "Baseline"
    case main     = "Main"
    case high     = "High"

    var id: String { rawValue }

    var vtProfileLevel: CFString {
        switch self {
        case .baseline: return kVTProfileLevel_H264_Baseline_AutoLevel
        case .main:     return kVTProfileLevel_H264_Main_AutoLevel
        case .high:     return kVTProfileLevel_H264_High_AutoLevel
        }
    }
}

/// Encoder configuration with sensible defaults for real-time display streaming
struct EncoderConfig {
    /// Output resolution (matches capture resolution by default)
    var width: Int32 = 1920
    var height: Int32 = 1080

    /// Target frame rate
    var fps: Double = 60.0

    /// Average bitrate in bits per second (default: 25 Mbps for good text clarity)
    var bitrateBps: Int = 25_000_000

    /// Maximum keyframe interval in frames (1 second at 60 FPS)
    var keyframeInterval: Int = 60

    /// H.264 profile (High for best quality/compression ratio)
    var profile: H264Profile = .high

    /// Real-time encoding mode — minimizes encode latency at cost of compression
    var realtime: Bool = true

    /// Disable B-frames — B-frames add latency due to frame reordering
    var allowFrameReordering: Bool = false

    /// Force hardware encoder (Apple Silicon VT hardware encoder)
    var hardwareAccelerated: Bool = true

    // ── Derived properties ──────────────────────────────────────────────────

    /// Bitrate in megabits per second (for UI display)
    var bitrateMbps: Double {
        get { Double(bitrateBps) / 1_000_000.0 }
        set { bitrateBps = Int(newValue * 1_000_000.0) }
    }

    /// Data rate limit (peak bitrate = 2× average) as [bytes/sec, seconds]
    var dataRateLimit: [CFNumber] {
        let peakBytesPerSec = Int(Double(bitrateBps) * 2.0 / 8.0)
        return [
            peakBytesPerSec as CFNumber,
            1 as CFNumber  // over 1 second window
        ]
    }
}

/// Snapshot of encoder performance metrics
struct EncoderStats {
    var encodedFrames: UInt64 = 0
    var encodedBytes: UInt64 = 0
    var keyframes: UInt64 = 0
    var avgEncodingLatencyMs: Double = 0
    var minEncodingLatencyMs: Double = 999
    var maxEncodingLatencyMs: Double = 0
    var outputBitrateMbps: Double = 0
    var isHardwareAccelerated: Bool = false
    var isEncoding: Bool = false
}
