// =============================================================================
// MacFinder.cpp — Background mDNS search and remembered Mac
// =============================================================================

#include "App/MacFinder.h"
#include "App/Log.h"

#include <Windows.h>
#include <fstream>

namespace {

std::string SettingsPath() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(nullptr, exePath, MAX_PATH);
    std::string path(exePath);
    auto pos = path.find_last_of("\\/");
    if (pos != std::string::npos) path = path.substr(0, pos + 1);
    return path + "receiver-settings.txt";
}

} // namespace

MacFinder::MacFinder() : started_(std::chrono::steady_clock::now()) {
    WSAStartup(MAKEWORD(2, 2), &wsa_);
    discovery_.setDiscoveryCallback([this](const DiscoveredService& svc) {
        if (svc.ipAddress.empty()) return;
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& mac : found_) {
            if (mac.name == svc.name) {
                mac.address = svc.ipAddress;   // moved to another address
                return;
            }
        }
        if (found_.empty()) firstFound_ = std::chrono::steady_clock::now();
        found_.push_back({ svc.name, svc.ipAddress });
        LOG_INFO("Discovery") << "Found " << FriendlyMacName(svc.name) << " at " << svc.ipAddress;
    });
    discovery_.startBrowsing();
}

MacFinder::~MacFinder() {
    discovery_.stopBrowsing();
    WSACleanup();
}

std::vector<FoundMac> MacFinder::found() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return found_;
}

double MacFinder::secondsSearching() const {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - started_).count();
}

double MacFinder::secondsSinceFirstFound() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (found_.empty()) return 0;
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - firstFound_).count();
}

std::string MacFinder::RememberedMac() {
    std::ifstream file(SettingsPath());
    std::string line;
    while (std::getline(file, line)) {
        if (line.rfind("last_mac=", 0) == 0) return line.substr(9);
    }
    return {};
}

void MacFinder::RememberMac(const std::string& serviceName) {
    std::ofstream file(SettingsPath(), std::ios::trunc);
    file << "last_mac=" << serviceName << "\n";
}

std::string FindMacAddress(const std::string& serviceName, int timeoutMs) {
    MacFinder finder;
    for (int waited = 0; waited < timeoutMs; waited += 100) {
        for (const auto& mac : finder.found()) {
            if (serviceName.empty() || mac.name == serviceName) return mac.address;
        }
        Sleep(100);
    }
    return {};
}
