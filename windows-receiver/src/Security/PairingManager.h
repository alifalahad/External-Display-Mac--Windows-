// =============================================================================
// PairingManager.h — This receiver's identity and its pairing with a Mac
// =============================================================================
// Protocol v2 (see SecureChannel.h): the pairing key is derived by both sides
// during pairing — it never crosses the network. It is stored next to the
// .exe in pairing.dat, encrypted with DPAPI for the current Windows user.
// The device ID (not secret) lets the Mac recognise this receiver.
//
// Thread-safe: the network thread reads the key during the handshake.
// =============================================================================

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

class PairingManager {
public:
    /// Load the device ID and any stored pairing. A receiver without a pairing
    /// gets a fresh device ID, so a Mac that remembers an old key pairs anew.
    void initialize();

    std::string getDeviceId() const;
    bool isPaired() const;

    /// Stored 32-byte pairing key (empty if not paired)
    std::vector<uint8_t> getPairingKey() const;
    std::string pairedServerName() const;

    void savePairing(const std::string& serverName, const std::vector<uint8_t>& key);

    /// Forget the pairing and switch to a fresh device ID
    void clearPairing();

private:
    std::string filePath(const char* name) const;
    void loadPairing();
    void createDeviceId();

    mutable std::mutex m_mutex;
    std::string m_deviceId;
    std::vector<uint8_t> m_pairingKey;
    std::string m_pairedServerName;
};
