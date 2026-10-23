// =============================================================================
// PairingManager.cpp — Device ID and DPAPI-protected pairing key
// =============================================================================

#include "Security/PairingManager.h"
#include "Security/Crypto.h"
#include "App/Log.h"

#include <Windows.h>

#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>

namespace {

std::string ToHex(const std::vector<uint8_t>& bytes) {
    std::ostringstream oss;
    for (uint8_t b : bytes) oss << std::hex << std::setw(2) << std::setfill('0') << int(b);
    return oss.str();
}

std::vector<uint8_t> FromHex(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>(std::strtol(hex.substr(i, 2).c_str(), nullptr, 16)));
    }
    return bytes;
}

} // namespace

void PairingManager::initialize() {
    // Protocol v1 stored a key that had been sent over the network in
    // plaintext: never use it again
    if (DeleteFileA(filePath("paired_server.dat").c_str())) {
        LOG_WARN("Pairing") << "Removed the old (protocol v1) pairing — pair with the Mac again";
    }

    loadPairing();
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_pairingKey.empty()) {
        std::ifstream file(filePath("device_id.dat"));
        if (file) std::getline(file, m_deviceId);
    }
    if (m_deviceId.empty()) createDeviceId();
    LOG_INFO("Pairing") << "Device ID " << m_deviceId.substr(0, 8) << "…, "
                        << (m_pairingKey.empty() ? "not paired yet" : "paired with " + m_pairedServerName);
}

std::string PairingManager::getDeviceId() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_deviceId;
}

bool PairingManager::isPaired() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pairingKey.size() == 32;
}

std::vector<uint8_t> PairingManager::getPairingKey() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pairingKey;
}

std::string PairingManager::pairedServerName() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pairedServerName;
}

void PairingManager::savePairing(const std::string& serverName, const std::vector<uint8_t>& key) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pairingKey = key;
    m_pairedServerName = serverName;

    std::string text = "SERVER=" + serverName + "\nKEY=" + ToHex(key) + "\n";
    crypto::Bytes sealed;
    if (!crypto::DpapiProtect(crypto::Bytes(text.begin(), text.end()), sealed)) {
        LOG_ERROR("Pairing") << "Could not encrypt the pairing key (DPAPI) — pairing kept for this session only";
        SecureZeroMemory(&text[0], text.size());
        return;
    }
    SecureZeroMemory(&text[0], text.size());
    std::ofstream file(filePath("pairing.dat"), std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(sealed.data()), static_cast<std::streamsize>(sealed.size()));
    if (!file) {
        LOG_ERROR("Pairing") << "Could not write pairing.dat next to the .exe";
        return;
    }
    LOG_INFO("Pairing") << "Paired with " << serverName;
}

void PairingManager::clearPairing() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pairingKey.clear();
    m_pairedServerName.clear();
    DeleteFileA(filePath("pairing.dat").c_str());
    createDeviceId();
    LOG_INFO("Pairing") << "Pairing cleared (new device ID " << m_deviceId.substr(0, 8) << "…)";
}

std::string PairingManager::filePath(const char* name) const {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path(exePath);
    auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos) path = path.substr(0, pos + 1);
    return path + name;
}

void PairingManager::loadPairing() {
    std::ifstream file(filePath("pairing.dat"), std::ios::binary);
    if (!file) return;
    crypto::Bytes sealed((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    crypto::Bytes plain;
    if (!crypto::DpapiUnprotect(sealed, plain)) {
        // Another Windows user, or a copy from another PC: DPAPI can't open it
        LOG_WARN("Pairing") << "pairing.dat cannot be decrypted for this Windows user — pair again";
        return;
    }

    std::istringstream text(std::string(plain.begin(), plain.end()));
    SecureZeroMemory(plain.data(), plain.size());
    std::string line, server, keyHex;
    while (std::getline(text, line)) {
        if (line.rfind("SERVER=", 0) == 0) server = line.substr(7);
        else if (line.rfind("KEY=", 0) == 0) keyHex = line.substr(4);
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    if (keyHex.size() == 64) {
        m_pairingKey = FromHex(keyHex);
        m_pairedServerName = server;
    }
}

void PairingManager::createDeviceId() {
    // Called with m_mutex held. Random UUID-style ID (not secret).
    uint8_t random[16];
    if (crypto::Random(random, sizeof(random))) {
        std::ostringstream oss;
        for (int i = 0; i < 16; i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10) oss << "-";
            oss << std::hex << std::setw(2) << std::setfill('0') << int(random[i]);
        }
        m_deviceId = oss.str();
    } else {
        char name[256];
        DWORD size = sizeof(name);
        GetComputerNameA(name, &size);
        m_deviceId = std::string(name) + "-" + std::to_string(GetTickCount64());
    }
    std::ofstream out(filePath("device_id.dat"), std::ios::trunc);
    out << m_deviceId;
}
