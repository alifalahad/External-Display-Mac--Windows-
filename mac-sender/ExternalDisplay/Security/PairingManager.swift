// =============================================================================
// PairingManager.swift — Paired receivers and pairing approval (protocol v2)
// =============================================================================
// Pairing (see SecureChannel.swift):
//   1. An unknown receiver connects → a request appears in the app with the
//      6-digit verification code both sides derived from the handshake.
//   2. The user types the code on Windows. Windows refuses a wrong code; a
//      right one is confirmed to the Mac (HMAC over the handshake).
//   3. The user clicks "Pair" here. Only now is the receiver trusted.
// The pairing key is derived on both sides — it never crosses the network.
//
// Storage: ~/Library/Application Support/ExternalDisplay/pairings-v2.json,
// owner-only (0600). The Keychain would be the choice for a signed release
// build; ad-hoc signed development builds get a Keychain prompt after every
// rebuild (see docs/SECURITY.md). Version 1 pairings are deleted: their keys
// were sent over the network in plaintext.
// =============================================================================

import Foundation

/// A pairing waiting for the user (UI state)
struct PairingRequest: Equatable {
    let deviceId: String
    let deviceName: String
    let verificationCode: String     // shown to the user only, never logged
    var windowsConfirmed: Bool       // Windows entered the right code
}

final class PairingManager: ObservableObject {

    // ── Published state (main thread) ───────────────────────────────────────

    /// The pairing the user is asked to approve (nil when none)
    @Published private(set) var request: PairingRequest?

    /// Paired receivers, for the UI (name, id, date)
    @Published private(set) var pairedDevices: [PairedDeviceInfo] = []

    struct PairedDeviceInfo: Identifiable, Equatable {
        let id: String
        let name: String
        let pairedAt: Date
    }

    /// Called on the main thread when the user approves or rejects `request`
    var onDecision: ((_ deviceId: String, _ approved: Bool) -> Void)?

    // ── Storage ─────────────────────────────────────────────────────────────

    private struct StoredDevice: Codable {
        let name: String
        let pairingKey: Data     // 32 bytes, derived during pairing
        let pairedAt: Date
    }

    private var devices: [String: StoredDevice] = [:]   // keyed by device ID
    private let lock = NSLock()                          // devices: network queue + main
    private let directory: URL

    init(directory: URL? = nil) {
        self.directory = directory ?? FileManager.default
            .urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("ExternalDisplay", isDirectory: true)
        removeLegacyPairings()
        load()
        Log.info(.pairing, "Loaded \(devices.count) paired receiver(s)")
    }

    // ── Queries (any thread) ────────────────────────────────────────────────

    func pairingKey(for deviceId: String) -> Data? {
        lock.lock()
        defer { lock.unlock() }
        return deviceId.isEmpty ? nil : devices[deviceId]?.pairingKey
    }

    // ── Pairing requests (network queue → main) ─────────────────────────────

    func beginRequest(deviceId: String, deviceName: String, verificationCode: String) {
        let request = PairingRequest(deviceId: deviceId, deviceName: deviceName,
                                     verificationCode: verificationCode, windowsConfirmed: false)
        DispatchQueue.main.async { [weak self] in self?.request = request }
        Log.info(.pairing, "Pairing request from '\(deviceName)' — waiting for the code on Windows")
    }

    func markWindowsConfirmed(deviceId: String) {
        DispatchQueue.main.async { [weak self] in
            guard let self, var current = self.request, current.deviceId == deviceId else { return }
            current.windowsConfirmed = true
            self.request = current
        }
        Log.info(.pairing, "Windows entered the correct verification code — waiting for approval")
    }

    func cancelRequest() {
        DispatchQueue.main.async { [weak self] in self?.request = nil }
    }

    // ── User decision (main thread) ─────────────────────────────────────────

    func approve() {
        guard let current = request, current.windowsConfirmed else { return }
        request = nil
        onDecision?(current.deviceId, true)
    }

    func reject() {
        guard let current = request else { return }
        request = nil
        onDecision?(current.deviceId, false)
    }

    // ── Storing pairings ────────────────────────────────────────────────────

    func savePairing(deviceId: String, deviceName: String, pairingKey: Data) {
        lock.lock()
        devices[deviceId] = StoredDevice(name: deviceName, pairingKey: pairingKey, pairedAt: Date())
        lock.unlock()
        save()
        Log.info(.pairing, "Paired with '\(deviceName)'")
    }

    /// Forget a receiver: it must pair again
    func unpair(deviceId: String) {
        lock.lock()
        let removed = devices.removeValue(forKey: deviceId)
        lock.unlock()
        if let removed {
            save()
            Log.info(.pairing, "Forgot '\(removed.name)'")
        }
    }

    // ── Persistence ─────────────────────────────────────────────────────────

    private var storeURL: URL { directory.appendingPathComponent("pairings-v2.json") }

    private func load() {
        guard let data = try? Data(contentsOf: storeURL),
              let decoded = try? JSONDecoder().decode([String: StoredDevice].self, from: data) else {
            publishDeviceList()
            return
        }
        lock.lock()
        devices = decoded.filter { $0.value.pairingKey.count == 32 }
        lock.unlock()
        publishDeviceList()
    }

    private func save() {
        lock.lock()
        let snapshot = devices
        lock.unlock()
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let data = try JSONEncoder().encode(snapshot)
            // Created owner-only before any key is written into it
            FileManager.default.createFile(atPath: storeURL.path, contents: nil,
                                           attributes: [.posixPermissions: 0o600])
            try data.write(to: storeURL, options: [.atomic])
            try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: storeURL.path)
        } catch {
            Log.error(.pairing, "Saving pairings failed: \(error.localizedDescription)")
        }
        publishDeviceList()
    }

    private func publishDeviceList() {
        lock.lock()
        let list = devices.map { PairedDeviceInfo(id: $0.key, name: $0.value.name, pairedAt: $0.value.pairedAt) }
            .sorted { $0.pairedAt > $1.pairedAt }
        lock.unlock()
        DispatchQueue.main.async { [weak self] in self?.pairedDevices = list }
    }

    private func removeLegacyPairings() {
        let legacy = directory.appendingPathComponent("paired_devices.json")
        if FileManager.default.fileExists(atPath: legacy.path) {
            try? FileManager.default.removeItem(at: legacy)
            Log.warning(.pairing, "Removed protocol v1 pairings (their keys were sent unencrypted) — pair again")
        }
    }
}
