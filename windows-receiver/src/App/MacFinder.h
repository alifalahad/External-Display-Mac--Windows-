#pragma once
// =============================================================================
// MacFinder.h — Find Mac senders on the network (mDNS) in the background
// =============================================================================
// The receiver window shows what is found; nothing blocks the UI thread.
// The Mac the user picks is remembered (receiver-settings.txt next to the
// .exe) and chosen automatically next time.
// =============================================================================

#include <winsock2.h>   // before anything that includes windows.h

#include "App/StatusText.h"
#include "Discovery/ServiceDiscovery.h"

#include <chrono>
#include <mutex>
#include <string>
#include <vector>

class MacFinder {
public:
    MacFinder();
    ~MacFinder();
    MacFinder(const MacFinder&) = delete;
    MacFinder& operator=(const MacFinder&) = delete;

    /// Macs found so far (with an IPv4 address), in discovery order
    std::vector<FoundMac> found() const;

    double secondsSearching() const;
    /// Seconds since the first Mac answered (0 if none yet)
    double secondsSinceFirstFound() const;

    static std::string RememberedMac();
    static void RememberMac(const std::string& serviceName);

private:
    WSADATA wsa_{};
    ServiceDiscovery discovery_;
    mutable std::mutex mutex_;
    std::vector<FoundMac> found_;
    std::chrono::steady_clock::time_point started_;
    std::chrono::steady_clock::time_point firstFound_{};
};

/// Look up one Mac's current address (it may have changed: DHCP, other
/// network). Blocks up to timeoutMs; returns "" if it does not answer.
std::string FindMacAddress(const std::string& serviceName, int timeoutMs = 5000);
