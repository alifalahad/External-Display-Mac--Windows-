// =============================================================================
// PairingManager.swift — Secure device pairing for External Display
// =============================================================================
// Manages PIN-based pairing and HMAC challenge-response authentication.
//
// First connection:
//   1. Mac generates 6-digit PIN, shows in UI
//   2. Windows user enters PIN
//   3. If correct, Mac generates 256-bit pairing key
//   4. Key saved on both sides for future auto-authentication
//
// Reconnection:
//   1. Mac sends random nonce
//   2. Windows computes HMAC-SHA256(nonce, pairingKey) and sends back
//   3. Mac verifies → auto-authenticated
//
// Uses CryptoKit for HMAC-SHA256 and SecRandomCopyBytes for secure RNG.
// =============================================================================

import Foundation
import CryptoKit

final class PairingManager: ObservableObject {

    // ── Published state (for UI) ────────────────────────────────────────────

    /// Currently displayed PIN (nil when not pairing)
    @Published var currentPIN: String?

    /// Name of the device being paired
    @Published var pairingDeviceName: String?

    // ── Internal state ──────────────────────────────────────────────────────

    private var pairedDevices: [String: PairedDevice] = [:]  // keyed by deviceId
    private var pendingPIN: String?
    private var pendingPairingKey: Data?
    private var pendingNonce: Data?

    /// Codable struct for persistence
    struct PairedDevice: Codable {
        let name: String
        let pairingKey: Data     // 32 bytes
        let pairedAt: Date
    }

    // ── Lifecycle ───────────────────────────────────────────────────────────

    init() {
        loadPairedDevices()
        print("[Pairing] Loaded \(pairedDevices.count) paired device(s)")
    }

    // ── Pairing Check ───────────────────────────────────────────────────────

    /// Check if a device with the given ID is already paired
    func isPaired(deviceId: String) -> Bool {
        return !deviceId.isEmpty && pairedDevices[deviceId] != nil
    }

    // ── PIN Pairing (First Time) ────────────────────────────────────────────

    /// Generate a new 6-digit PIN for pairing a new device.
    /// Returns the PIN string. Shows it in the UI.
    func generatePIN(forDevice deviceName: String) -> String {
        let pin = String(format: "%06d", Int.random(in: 0...999999))

        pendingPIN = pin
        pendingPairingKey = generateRandomBytes(32)

        DispatchQueue.main.async { [weak self] in
            self?.currentPIN = pin
            self?.pairingDeviceName = deviceName
        }

        print("[Pairing] PIN generated for '\(deviceName)': \(pin)")
        return pin
    }

    /// Validate the PIN entered by the Windows user.
    /// Returns the 256-bit pairing key on success, nil on failure.
    func validatePIN(_ enteredPIN: String, deviceId: String, deviceName: String) -> Data? {
        guard let expectedPIN = pendingPIN,
              let key = pendingPairingKey,
              enteredPIN == expectedPIN else {
            print("[Pairing] ❌ PIN rejected (entered: '\(enteredPIN)')")
            return nil
        }

        // Save the pairing
        pairedDevices[deviceId] = PairedDevice(
            name: deviceName,
            pairingKey: key,
            pairedAt: Date()
        )
        savePairedDevices()

        // Clear pending state
        pendingPIN = nil
        pendingPairingKey = nil

        DispatchQueue.main.async { [weak self] in
            self?.currentPIN = nil
            self?.pairingDeviceName = nil
        }

        print("[Pairing] ✅ Paired with '\(deviceName)' (id: \(deviceId.prefix(16))…)")
        return key
    }

    /// Cancel an in-progress PIN pairing
    func cancelPairing() {
        pendingPIN = nil
        pendingPairingKey = nil
        DispatchQueue.main.async { [weak self] in
            self?.currentPIN = nil
            self?.pairingDeviceName = nil
        }
    }

    // ── HMAC Challenge-Response (Reconnection) ──────────────────────────────

    /// Generate a random 32-byte nonce for HMAC challenge.
    /// Stores it internally for later validation.
    func generateNonce() -> Data {
        let nonce = generateRandomBytes(32)
        pendingNonce = nonce
        return nonce
    }

    /// Validate the HMAC response from a previously paired device.
    func validateHMAC(deviceId: String, receivedHMAC: Data) -> Bool {
        guard let device = pairedDevices[deviceId],
              let nonce = pendingNonce else {
            print("[Pairing] ❌ HMAC validation failed — device not paired or no nonce")
            return false
        }

        // Compute expected HMAC-SHA256(nonce, pairingKey)
        let key = SymmetricKey(data: device.pairingKey)
        let expectedHMAC = HMAC<SHA256>.authenticationCode(for: nonce, using: key)
        let expectedData = Data(expectedHMAC)

        pendingNonce = nil  // Consume the nonce

        if receivedHMAC == expectedData {
            print("[Pairing] ✅ HMAC verified for '\(device.name)'")
            return true
        } else {
            print("[Pairing] ❌ HMAC mismatch for '\(device.name)'")
            return false
        }
    }

    /// Get the pairing key for a device (for sending in PairAccept)
    func getPairingKey(for deviceId: String) -> Data? {
        return pairedDevices[deviceId]?.pairingKey
    }

    // ── Device Management ───────────────────────────────────────────────────

    /// Remove a paired device (forget)
    func unpairDevice(deviceId: String) {
        if let device = pairedDevices.removeValue(forKey: deviceId) {
            savePairedDevices()
            print("[Pairing] Removed pairing for '\(device.name)'")
        }
    }

    /// Get list of all paired devices
    func listPairedDevices() -> [(id: String, name: String, date: Date)] {
        return pairedDevices.map { ($0.key, $0.value.name, $0.value.pairedAt) }
    }

    // ── Crypto Helpers ──────────────────────────────────────────────────────

    private func generateRandomBytes(_ count: Int) -> Data {
        var bytes = [UInt8](repeating: 0, count: count)
        let status = SecRandomCopyBytes(kSecRandomDefault, count, &bytes)
        if status != errSecSuccess {
            // Fallback — less secure but functional
            bytes = (0..<count).map { _ in UInt8.random(in: 0...255) }
        }
        return Data(bytes)
    }

    // ── Persistence ─────────────────────────────────────────────────────────

    private var storageURL: URL {
        let appSupport = FileManager.default.urls(
            for: .applicationSupportDirectory, in: .userDomainMask
        ).first!.appendingPathComponent("ExternalDisplay")

        // Create directory if it doesn't exist
        try? FileManager.default.createDirectory(at: appSupport, withIntermediateDirectories: true)

        return appSupport.appendingPathComponent("paired_devices.json")
    }

    private func loadPairedDevices() {
        guard let data = try? Data(contentsOf: storageURL),
              let devices = try? JSONDecoder().decode([String: PairedDevice].self, from: data) else {
            return
        }
        pairedDevices = devices
    }

    private func savePairedDevices() {
        let encoder = JSONEncoder()
        encoder.outputFormatting = .prettyPrinted
        guard let data = try? encoder.encode(pairedDevices) else { return }
        try? data.write(to: storageURL)
    }
}
