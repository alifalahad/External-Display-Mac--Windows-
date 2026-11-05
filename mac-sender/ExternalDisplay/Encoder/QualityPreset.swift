// =============================================================================
// QualityPreset.swift — What the user picks; adaptive quality works within it
// =============================================================================
// The adaptive controller (AdaptiveQuality.swift) still follows the network,
// but never above the preset's ceiling. Battery Saver also halves the frame
// rate: half the capture, encode and Wi-Fi work. High streams 120 FPS where
// the PC's screen and the encoder can (FrameRate.swift, step 7).
// =============================================================================

import Foundation

enum QualityPreset: String, CaseIterable, Identifiable {
    case batterySaver = "Battery Saver"
    case balanced     = "Balanced"
    case high         = "High"

    var id: String { rawValue }

    /// The most frames per second; FrameRate.choose() decides what runs
    var maxFps: Int {
        switch self {
        case .batterySaver: return 30
        case .balanced:     return 60
        case .high:         return 120
        }
    }

    var minBitrateBps: Int { self == .batterySaver ? 4_000_000 : 8_000_000 }

    var maxBitrateBps: Int {
        switch self {
        case .batterySaver: return 12_000_000
        case .balanced:     return 25_000_000
        case .high:         return 40_000_000
        }
    }

    /// Where a new stream starts before the first network report
    var startBitrateBps: Int {
        switch self {
        case .batterySaver: return 8_000_000
        case .balanced:     return 20_000_000
        case .high:         return 25_000_000
        }
    }

    var summary: String {
        (maxFps > 60 ? "up to \(maxFps)" : "\(maxFps)") + " FPS · up to \(maxBitrateBps / 1_000_000) Mbps"
    }

    private static let key = "qualityPreset"

    static var saved: QualityPreset {
        get { QualityPreset(rawValue: UserDefaults.standard.string(forKey: key) ?? "") ?? .balanced }
        set { UserDefaults.standard.set(newValue.rawValue, forKey: key) }
    }
}
