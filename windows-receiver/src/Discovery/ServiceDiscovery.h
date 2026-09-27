// =============================================================================
// ServiceDiscovery.h — LAN Service Discovery (Windows DNS-SD)
// =============================================================================
// Browses for Mac senders advertising "_externaldisplay._tcp" via Bonjour/mDNS.
// Uses Windows native DNS-SD APIs (available since Windows 10 1809).
//
// Discovered services are reported via callback with:
//   - Service name (e.g. "ExternalDisplay-Alif's MacBook")
//   - IP address (resolved IPv4)
//   - Port number
//
// Usage:
//   ServiceDiscovery discovery;
//   discovery.setDiscoveryCallback([](const DiscoveredService& svc) { ... });
//   discovery.startBrowsing();
//   // ... later:
//   discovery.stopBrowsing();
// =============================================================================

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <map>

// ── Discovered Service ──────────────────────────────────────────────────────

struct DiscoveredService {
    std::string name;       // Bonjour service name
    std::string host;       // Resolved hostname or IP
    std::string ipAddress;  // Resolved IPv4 address (e.g. "192.168.1.103")
    uint16_t    port = 0;   // TCP port
    bool        resolved = false;
};

// ── Service Discovery ───────────────────────────────────────────────────────

class ServiceDiscovery {
public:
    /// Callback when a service is discovered or updated
    using DiscoveryCallback = std::function<void(const DiscoveredService& service)>;

    /// Callback when a service is removed (went offline)
    using ServiceRemovedCallback = std::function<void(const std::string& serviceName)>;

    ServiceDiscovery();
    ~ServiceDiscovery();

    /// Start browsing for External Display senders on the LAN.
    /// Uses UDP multicast (mDNS) to discover services.
    void startBrowsing();

    /// Stop browsing
    void stopBrowsing();

    /// Check if currently browsing
    bool isBrowsing() const { return m_browsing.load(); }

    /// Set callback for discovered services
    void setDiscoveryCallback(DiscoveryCallback cb) { m_discoveryCallback = std::move(cb); }

    /// Set callback for removed services
    void setServiceRemovedCallback(ServiceRemovedCallback cb) { m_removedCallback = std::move(cb); }

    /// Get all currently known services (thread-safe)
    std::vector<DiscoveredService> getServices() const;

    /// Force a refresh — re-send mDNS queries
    void refresh();

private:
    // ── mDNS Implementation ─────────────────────────────────────────────────

    /// Background thread: sends mDNS queries and listens for responses
    void browseThread();

    /// Parse an mDNS response packet
    void parseMDNSResponse(const uint8_t* data, size_t length, const std::string& fromIP);

    /// Parse a DNS name from an mDNS packet (handles compression pointers)
    std::string parseDNSName(const uint8_t* packet, size_t packetLen,
                              size_t& offset, int depth = 0);

    /// Build an mDNS query packet for _externaldisplay._tcp.local.
    std::vector<uint8_t> buildMDNSQuery();

    /// Try to resolve service details via additional mDNS queries
    void resolveService(const std::string& instanceName);

    // ── State ───────────────────────────────────────────────────────────────

    std::atomic<bool> m_browsing{false};
    std::thread m_browseThread;

    mutable std::mutex m_servicesMutex;
    std::map<std::string, DiscoveredService> m_services;

    DiscoveryCallback m_discoveryCallback;
    ServiceRemovedCallback m_removedCallback;

    // Trigger refresh from main thread
    std::atomic<bool> m_refreshRequested{false};

    // mDNS constants
    static constexpr uint16_t MDNS_PORT = 5353;
    static constexpr const char* MDNS_MULTICAST_ADDR = "224.0.0.251";
    static constexpr const char* SERVICE_TYPE = "_externaldisplay._tcp.local.";
    static constexpr int BROWSE_INTERVAL_MS = 5000; // Re-query every 5 seconds
};
