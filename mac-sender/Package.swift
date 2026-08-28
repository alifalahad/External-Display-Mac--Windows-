// swift-tools-version: 5.9
// =============================================================================
// Package.swift — Mac Sender (External Display)
// =============================================================================
// Build: swift build
// Run:   swift run ExternalDisplay
// Xcode: open Package.swift  (opens in Xcode, press ⌘R to run)
// =============================================================================

import PackageDescription

let package = Package(
    name: "ExternalDisplay",
    platforms: [
        .macOS(.v13)  // macOS Ventura+ (ScreenCaptureKit 12.3+, but 13 for stable APIs)
    ],
    targets: [
        .executableTarget(
            name: "ExternalDisplay",
            path: "ExternalDisplay"
        )
    ]
)
