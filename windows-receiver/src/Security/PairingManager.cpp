// =============================================================================
// PairingManager.cpp — Secure device pairing for Windows receiver
// =============================================================================

#include "PairingManager.h"

#include <Windows.h>
#include <bcrypt.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "bcrypt.lib")

// ── Lifecycle ───────────────────────────────────────────────────────────────

PairingManager::PairingManager() {}

void PairingManager::initialize() {
    loadOrCreateDeviceId();
    loadPairing();
    std::cout << "[Pairing] Device ID: " << m_deviceId.substr(0, 16) << "..." << std::endl;
    if (isPaired()) {
        std::cout << "[Pairing] Paired with: " << m_pairedServerName << std::endl;
    } else {
        std::cout << "[Pairing] Not yet paired" << std::endl;
    }
}

// ── HMAC-SHA256 ─────────────────────────────────────────────────────────────

bool PairingManager::computeHMAC(const uint8_t* nonce, size_t nonceLen,
                                  uint8_t output[32]) {
    if (m_pairingKey.size() != 32) return false;

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    bool success = false;

    NTSTATUS status = BCryptOpenAlgorithmProvider(
        &hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    if (!BCRYPT_SUCCESS(status)) return false;

    status = BCryptCreateHash(
        hAlg, &hHash, nullptr, 0,
        (PUCHAR)m_pairingKey.data(), (ULONG)m_pairingKey.size(), 0);
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptHashData(hHash, (PUCHAR)nonce, (ULONG)nonceLen, 0);
        if (BCRYPT_SUCCESS(status)) {
            status = BCryptFinishHash(hHash, output, 32, 0);
            success = BCRYPT_SUCCESS(status);
        }
        BCryptDestroyHash(hHash);
    }

    BCryptCloseAlgorithmProvider(hAlg, 0);
    return success;
}

// ── Random Number Generation ────────────────────────────────────────────────

bool PairingManager::generateRandom(uint8_t* buffer, size_t size) {
    return BCRYPT_SUCCESS(
        BCryptGenRandom(nullptr, buffer, (ULONG)size, BCRYPT_USE_SYSTEM_PREFERRED_RNG));
}

// ── PIN Request ─────────────────────────────────────────────────────────────

bool PairingManager::requestPIN(std::string& outPIN) {
    if (m_pinRequestCallback) {
        return m_pinRequestCallback(outPIN);
    }
    return false;
}

// ── Pairing Data Persistence ────────────────────────────────────────────────

void PairingManager::savePairing(const std::string& serverName,
                                  const uint8_t* key, size_t keyLen) {
    m_pairingKey.assign(key, key + keyLen);
    m_pairedServerName = serverName;
    savePairingToFile();
    std::cout << "[Pairing] \u2705 Paired with: " << serverName << std::endl;
}

void PairingManager::clearPairing() {
    m_pairingKey.clear();
    m_pairedServerName.clear();

    std::string path = getPairingFilePath();
    DeleteFileA(path.c_str());
    std::cout << "[Pairing] Pairing data cleared" << std::endl;
}

std::string PairingManager::getPairingFilePath() const {
    // Store next to the executable
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path(exePath);
    auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos) path = path.substr(0, pos + 1);
    return path + "paired_server.dat";
}

std::string PairingManager::getDeviceIdFilePath() const {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path(exePath);
    auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos) path = path.substr(0, pos + 1);
    return path + "device_id.dat";
}

void PairingManager::loadPairing() {
    std::ifstream file(getPairingFilePath());
    if (!file.is_open()) return;

    std::string line;
    std::string serverName;
    std::string keyHex;

    while (std::getline(file, line)) {
        if (line.substr(0, 7) == "SERVER=") {
            serverName = line.substr(7);
        } else if (line.substr(0, 4) == "KEY=") {
            keyHex = line.substr(4);
        }
    }

    if (!serverName.empty() && keyHex.size() == 64) {
        m_pairedServerName = serverName;
        m_pairingKey = hexToBytes(keyHex);
    }
}

void PairingManager::savePairingToFile() {
    std::ofstream file(getPairingFilePath());
    if (!file.is_open()) {
        std::cerr << "[Pairing] Failed to save pairing data" << std::endl;
        return;
    }

    file << "SERVER=" << m_pairedServerName << std::endl;
    file << "KEY=" << bytesToHex(m_pairingKey.data(), m_pairingKey.size()) << std::endl;
}

// ── Device ID ───────────────────────────────────────────────────────────────

void PairingManager::loadOrCreateDeviceId() {
    // Try to load existing device ID
    std::ifstream file(getDeviceIdFilePath());
    if (file.is_open()) {
        std::getline(file, m_deviceId);
        if (!m_deviceId.empty()) return;
    }

    // Generate a new device ID (random UUID-like string)
    uint8_t randomBytes[16];
    if (generateRandom(randomBytes, 16)) {
        // Format as UUID: xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
        std::ostringstream oss;
        for (int i = 0; i < 16; i++) {
            if (i == 4 || i == 6 || i == 8 || i == 10) oss << "-";
            oss << std::hex << std::setw(2) << std::setfill('0') << (int)randomBytes[i];
        }
        m_deviceId = oss.str();
    } else {
        // Fallback: use computer name + time
        char compName[256];
        DWORD size = sizeof(compName);
        GetComputerNameA(compName, &size);
        m_deviceId = std::string(compName) + "-" + std::to_string(GetTickCount64());
    }

    // Save for future runs
    std::ofstream outFile(getDeviceIdFilePath());
    if (outFile.is_open()) {
        outFile << m_deviceId;
    }
}

// ── Hex Conversion Helpers ──────────────────────────────────────────────────

std::string PairingManager::bytesToHex(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; i++) {
        oss << std::hex << std::setw(2) << std::setfill('0') << (int)data[i];
    }
    return oss.str();
}

std::vector<uint8_t> PairingManager::hexToBytes(const std::string& hex) {
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        uint8_t byte = (uint8_t)std::strtol(hex.substr(i, 2).c_str(), nullptr, 16);
        bytes.push_back(byte);
    }
    return bytes;
}
