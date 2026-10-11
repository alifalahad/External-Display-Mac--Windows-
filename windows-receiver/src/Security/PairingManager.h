// =============================================================================
// PairingManager.h — Secure device pairing for Windows receiver
// =============================================================================
// Handles PIN-based pairing and HMAC challenge-response authentication.
// Uses Windows BCrypt API for HMAC-SHA256 and random number generation.
//
// Stores pairing data in a file next to the executable.
// =============================================================================

#pragma once

#include <string>
#include <cstdint>
#include <vector>

class PairingManager {
public:
    PairingManager();
    ~PairingManager() = default;

    /// Initialize: loads existing pairing data and the device ID.
    /// An unpaired receiver always gets a fresh device ID, so a Mac that still
    /// remembers an old key falls back to PIN pairing instead of HMAC.
    void initialize();

    /// Get this device's unique ID (generated on first run, persisted)
    const std::string& getDeviceId() const { return m_deviceId; }

    /// Check if we have a stored pairing key for a server
    bool isPaired() const { return !m_pairingKey.empty(); }

    /// Get the stored pairing key (32 bytes)
    const std::vector<uint8_t>& getPairingKey() const { return m_pairingKey; }

    /// Store a new pairing key (received from Mac after PIN verification)
    void savePairing(const std::string& serverName, const uint8_t* key, size_t keyLen);

    /// Compute HMAC-SHA256(nonce, pairingKey) for challenge-response
    bool computeHMAC(const uint8_t* nonce, size_t nonceLen,
                     uint8_t output[32]);

    /// Generate random bytes using Windows CNG
    static bool generateRandom(uint8_t* buffer, size_t size);

    /// Remove stored pairing (unpair) and switch to a fresh device ID
    void clearPairing();

private:
    std::string m_deviceId;            // Persistent unique device identifier
    std::vector<uint8_t> m_pairingKey; // 32-byte pairing key
    std::string m_pairedServerName;    // Name of paired Mac server

    /// File path for pairing data
    std::string getPairingFilePath() const;
    std::string getDeviceIdFilePath() const;

    /// Load/save pairing data
    void loadPairing();
    void savePairingToFile();

    /// Load/save device ID
    void loadOrCreateDeviceId();
    void createDeviceId();

    /// Convert bytes to hex string and back
    static std::string bytesToHex(const uint8_t* data, size_t len);
    static std::vector<uint8_t> hexToBytes(const std::string& hex);
};
