// =============================================================================
// ClipboardSync.swift — Bidirectional clipboard synchronization
// =============================================================================
// Monitors the macOS pasteboard for changes and sends text to Windows.
// Receives clipboard text from Windows and sets it on the macOS pasteboard.
//
// Anti-loop protection:
//   - Tracks the last clipboard content we set (from remote)
//   - Only sends changes that originated locally
//   - Uses a hash-based dedup to prevent infinite bounce
// =============================================================================

import Foundation
import AppKit

final class ClipboardSync {

    // ── Configuration ───────────────────────────────────────────────────────

    /// How often to poll the pasteboard for changes (seconds)
    private let pollInterval: TimeInterval = 0.5

    /// Max clipboard text size to sync (prevent huge clipboard transfers)
    private let maxTextSize = 1_000_000  // 1 MB

    // ── Callbacks ───────────────────────────────────────────────────────────

    /// Called when local clipboard changes — provides UTF-8 text data to send
    var onClipboardChanged: ((Data) -> Void)?

    // ── State ───────────────────────────────────────────────────────────────

    private var pollTimer: Timer?
    private var isActive = false

    /// The pasteboard change count at last check
    private var lastChangeCount: Int = 0

    /// Hash of the last content we SET on the pasteboard (from remote)
    /// Used to prevent sending back what we just received
    private var lastRemoteContentHash: Int = 0

    /// Hash of the last content we SENT to the remote
    /// Used to prevent duplicate sends
    private var lastSentContentHash: Int = 0

    // ── Lifecycle ───────────────────────────────────────────────────────────

    /// Start monitoring the clipboard for changes
    func start() {
        guard !isActive else { return }
        isActive = true

        // Record current pasteboard state (don't send whatever is already there)
        lastChangeCount = NSPasteboard.general.changeCount

        // Poll on main thread (NSPasteboard requires it)
        pollTimer = Timer.scheduledTimer(withTimeInterval: pollInterval, repeats: true) { [weak self] _ in
            self?.checkForChanges()
        }

        print("[Clipboard] Monitoring started (poll interval: \(pollInterval)s)")
    }

    /// Stop monitoring
    func stop() {
        guard isActive else { return }
        isActive = false
        pollTimer?.invalidate()
        pollTimer = nil
        print("[Clipboard] Monitoring stopped")
    }

    // ── Receive from Remote ─────────────────────────────────────────────────

    /// Called when clipboard data arrives from Windows
    func receiveRemoteClipboard(_ data: Data) {
        guard let text = String(data: data, encoding: .utf8), !text.isEmpty else {
            print("[Clipboard] Received empty or invalid clipboard data")
            return
        }

        let hash = text.hashValue

        // Dedup: don't set if it's the same as what we just sent
        if hash == lastSentContentHash {
            return
        }

        // Set on macOS pasteboard
        let pb = NSPasteboard.general
        pb.clearContents()
        pb.setString(text, forType: .string)

        // Remember this hash so we don't echo it back
        lastRemoteContentHash = hash
        lastChangeCount = pb.changeCount

        print("[Clipboard] ← Received from Windows: \(text.prefix(50))\(text.count > 50 ? "…" : "") (\(data.count) bytes)")
    }

    // ── Poll for Local Changes ──────────────────────────────────────────────

    private func checkForChanges() {
        let pb = NSPasteboard.general
        let currentCount = pb.changeCount

        // No change since last poll
        guard currentCount != lastChangeCount else { return }
        lastChangeCount = currentCount

        // Get the text from pasteboard
        guard let text = pb.string(forType: .string), !text.isEmpty else { return }

        // Size guard
        guard text.count <= maxTextSize else {
            print("[Clipboard] Skipping oversized clipboard (\(text.count) chars)")
            return
        }

        let hash = text.hashValue

        // Anti-loop: skip if this is content we just received from remote
        if hash == lastRemoteContentHash {
            return
        }

        // Dedup: skip if we already sent this exact content
        if hash == lastSentContentHash {
            return
        }

        // Convert to UTF-8 data and send
        guard let data = text.data(using: .utf8) else { return }

        lastSentContentHash = hash
        onClipboardChanged?(data)

        print("[Clipboard] → Sending to Windows: \(text.prefix(50))\(text.count > 50 ? "…" : "") (\(data.count) bytes)")
    }

    deinit {
        stop()
    }
}
