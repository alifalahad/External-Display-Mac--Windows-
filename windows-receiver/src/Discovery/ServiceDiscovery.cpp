// =============================================================================
// ServiceDiscovery.cpp — LAN Service Discovery (mDNS implementation)
// =============================================================================
// Pure mDNS implementation — no external dependencies (no Bonjour SDK).
// Sends standard mDNS queries to 224.0.0.251:5353 and parses responses
// to discover _externaldisplay._tcp services on the local network.
//
// mDNS packet format follows RFC 6762 / RFC 1035.
// =============================================================================

#include "ServiceDiscovery.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <iostream>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")

// ── DNS Record Types ────────────────────────────────────────────────────────

namespace dns {
    constexpr uint16_t TYPE_A     = 1;    // IPv4 address
    constexpr uint16_t TYPE_PTR   = 12;   // Pointer (service enumeration)
    constexpr uint16_t TYPE_TXT   = 16;   // Text record
    constexpr uint16_t TYPE_AAAA  = 28;   // IPv6 address
    constexpr uint16_t TYPE_SRV   = 33;   // Service locator
    constexpr uint16_t CLASS_IN   = 1;    // Internet
    constexpr uint16_t CLASS_FLUSH = 0x8001; // Cache flush + IN
}

// ── Byte helpers (network byte order) ───────────────────────────────────────

static uint16_t readU16BE(const uint8_t* p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static void writeU16BE(std::vector<uint8_t>& buf, uint16_t val) {
    buf.push_back((uint8_t)(val >> 8));
    buf.push_back((uint8_t)(val & 0xFF));
}

// =============================================================================
// Lifecycle
// =============================================================================

ServiceDiscovery::ServiceDiscovery() {
    // Winsock should already be initialized by StreamReceiver
}

ServiceDiscovery::~ServiceDiscovery() {
    stopBrowsing();
}

void ServiceDiscovery::startBrowsing() {
    if (m_browsing.load()) return;
    m_browsing.store(true);
    m_browseThread = std::thread(&ServiceDiscovery::browseThread, this);
    std::cout << "[Discovery] Started browsing for External Display senders..." << std::endl;
}

void ServiceDiscovery::stopBrowsing() {
    m_browsing.store(false);
    if (m_browseThread.joinable()) {
        m_browseThread.join();
    }
    std::cout << "[Discovery] Stopped browsing" << std::endl;
}

std::vector<DiscoveredService> ServiceDiscovery::getServices() const {
    std::lock_guard<std::mutex> lock(m_servicesMutex);
    std::vector<DiscoveredService> result;
    result.reserve(m_services.size());
    for (auto& [name, svc] : m_services) {
        result.push_back(svc);
    }
    return result;
}

void ServiceDiscovery::refresh() {
    m_refreshRequested.store(true);
}

// =============================================================================
// mDNS Browse Thread
// =============================================================================

void ServiceDiscovery::browseThread() {
    // Create UDP socket for mDNS
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "[Discovery] Failed to create mDNS socket: " << WSAGetLastError() << std::endl;
        return;
    }

    // Allow address reuse (multiple mDNS listeners)
    BOOL reuseAddr = TRUE;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char*)&reuseAddr, sizeof(reuseAddr));

    // Bind to mDNS port
    sockaddr_in bindAddr{};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_port = htons(MDNS_PORT);
    bindAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        std::cerr << "[Discovery] Failed to bind mDNS socket: " << WSAGetLastError() << std::endl;
        closesocket(sock);
        return;
    }

    // Join mDNS multicast group on all interfaces
    ip_mreq mreq{};
    inet_pton(AF_INET, MDNS_MULTICAST_ADDR, &mreq.imr_multiaddr);
    mreq.imr_interface.s_addr = INADDR_ANY;

    if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq, sizeof(mreq)) == SOCKET_ERROR) {
        std::cerr << "[Discovery] Failed to join mDNS multicast group: " << WSAGetLastError() << std::endl;
        // Continue anyway — we can still send queries and get unicast replies
    }

    // Set receive timeout for interruptible loop
    DWORD timeout = 500; // 500ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

    // Multicast destination
    sockaddr_in mdnsAddr{};
    mdnsAddr.sin_family = AF_INET;
    mdnsAddr.sin_port = htons(MDNS_PORT);
    inet_pton(AF_INET, MDNS_MULTICAST_ADDR, &mdnsAddr.sin_addr);

    auto lastQuery = std::chrono::steady_clock::now() - std::chrono::seconds(10); // Force immediate query

    std::vector<uint8_t> recvBuf(65536);

    while (m_browsing.load()) {
        // Send periodic mDNS query
        auto now = std::chrono::steady_clock::now();
        float elapsed = std::chrono::duration<float>(now - lastQuery).count();
        if (elapsed >= (BROWSE_INTERVAL_MS / 1000.0f) || m_refreshRequested.exchange(false)) {
            auto query = buildMDNSQuery();
            sendto(sock, (const char*)query.data(), (int)query.size(), 0,
                   (sockaddr*)&mdnsAddr, sizeof(mdnsAddr));
            lastQuery = now;
        }

        // Receive mDNS responses
        sockaddr_in fromAddr{};
        int fromLen = sizeof(fromAddr);
        int n = recvfrom(sock, (char*)recvBuf.data(), (int)recvBuf.size(), 0,
                         (sockaddr*)&fromAddr, &fromLen);

        if (n > 0) {
            // Get sender IP
            char ipStr[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &fromAddr.sin_addr, ipStr, sizeof(ipStr));

            parseMDNSResponse(recvBuf.data(), (size_t)n, ipStr);
        }
    }

    // Leave multicast group
    setsockopt(sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, (char*)&mreq, sizeof(mreq));
    closesocket(sock);
}

// =============================================================================
// mDNS Query Builder
// =============================================================================

std::vector<uint8_t> ServiceDiscovery::buildMDNSQuery() {
    std::vector<uint8_t> packet;
    packet.reserve(256);

    // DNS Header (12 bytes)
    writeU16BE(packet, 0x0000);  // Transaction ID (0 for mDNS)
    writeU16BE(packet, 0x0000);  // Flags: standard query
    writeU16BE(packet, 0x0001);  // Questions: 1
    writeU16BE(packet, 0x0000);  // Answers: 0
    writeU16BE(packet, 0x0000);  // Authority: 0
    writeU16BE(packet, 0x0000);  // Additional: 0

    // Question: _externaldisplay._tcp.local. IN PTR
    // DNS name encoding: each label is preceded by its length byte
    // "_externaldisplay" = 16 chars
    const char* labels[] = {"_externaldisplay", "_tcp", "local"};
    for (const char* label : labels) {
        uint8_t len = (uint8_t)strlen(label);
        packet.push_back(len);
        for (uint8_t i = 0; i < len; i++) {
            packet.push_back((uint8_t)label[i]);
        }
    }
    packet.push_back(0x00);  // Root label (end of name)

    writeU16BE(packet, dns::TYPE_PTR);   // Query type: PTR
    writeU16BE(packet, dns::CLASS_IN);   // Query class: IN (Internet)

    return packet;
}

// =============================================================================
// mDNS Response Parser
// =============================================================================

std::string ServiceDiscovery::parseDNSName(const uint8_t* packet, size_t packetLen,
                                            size_t& offset, int depth) {
    if (depth > 10) return ""; // Prevent infinite recursion

    std::string name;

    while (offset < packetLen) {
        uint8_t labelLen = packet[offset];

        if (labelLen == 0) {
            // End of name
            offset++;
            break;
        }

        // Check for compression pointer (top 2 bits = 11)
        if ((labelLen & 0xC0) == 0xC0) {
            if (offset + 1 >= packetLen) break;
            size_t pointerOffset = ((labelLen & 0x3F) << 8) | packet[offset + 1];
            offset += 2; // Skip the 2-byte pointer

            // Follow the pointer
            size_t ptrOff = pointerOffset;
            std::string rest = parseDNSName(packet, packetLen, ptrOff, depth + 1);
            if (!name.empty()) name += ".";
            name += rest;
            return name; // Pointer terminates the name
        }

        // Regular label
        offset++;
        if (offset + labelLen > packetLen) break;

        if (!name.empty()) name += ".";
        name += std::string((const char*)&packet[offset], labelLen);
        offset += labelLen;
    }

    return name;
}

void ServiceDiscovery::parseMDNSResponse(const uint8_t* data, size_t length,
                                          const std::string& fromIP) {
    if (length < 12) return; // Too short for DNS header

    // Parse DNS header
    uint16_t flags = readU16BE(data + 2);
    bool isResponse = (flags & 0x8000) != 0;
    if (!isResponse) return; // We only care about responses

    uint16_t qdCount = readU16BE(data + 4);   // Questions
    uint16_t anCount = readU16BE(data + 6);   // Answers
    uint16_t nsCount = readU16BE(data + 8);   // Authority
    uint16_t arCount = readU16BE(data + 10);  // Additional

    size_t offset = 12; // Skip header

    // Skip questions section
    for (uint16_t i = 0; i < qdCount && offset < length; i++) {
        parseDNSName(data, length, offset); // Skip name
        if (offset + 4 > length) return;
        offset += 4; // Skip type + class
    }

    // Temporary storage for parsing all records together
    std::string foundInstanceName;
    std::string foundHostname;
    std::string foundIP;
    uint16_t foundPort = 0;

    // Parse all resource records (answers + authority + additional)
    uint16_t totalRecords = anCount + nsCount + arCount;

    for (uint16_t i = 0; i < totalRecords && offset < length; i++) {
        // Parse record name
        size_t nameStart = offset;
        std::string recordName = parseDNSName(data, length, offset);

        // Parse type, class, TTL, data length
        if (offset + 10 > length) break;
        uint16_t rtype  = readU16BE(data + offset); offset += 2;
        uint16_t rclass = readU16BE(data + offset); offset += 2;
        /* uint32_t ttl */ offset += 4; // Skip TTL
        uint16_t rdlength = readU16BE(data + offset); offset += 2;

        if (offset + rdlength > length) break;

        size_t rdataStart = offset;

        (void)rclass; // Suppress unused variable warning

        // ── PTR Record — Service enumeration ────────────────────────────
        if (rtype == dns::TYPE_PTR) {
            // Check if this is for our service type
            // recordName should be "_externaldisplay._tcp.local" (case-insensitive)
            std::string lower = recordName;
            std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

            if (lower.find("_externaldisplay._tcp") != std::string::npos) {
                // The RDATA is the instance name (e.g. "ExternalDisplay-Alif's Mac._externaldisplay._tcp.local")
                size_t ptrOffset = rdataStart;
                foundInstanceName = parseDNSName(data, length, ptrOffset);
                std::cout << "[Discovery] PTR: Found service instance: " << foundInstanceName << std::endl;
            }
        }

        // ── SRV Record — Service location (host + port) ────────────────
        else if (rtype == dns::TYPE_SRV) {
            if (rdlength >= 6) {
                // SRV RDATA: priority(2) + weight(2) + port(2) + target(name)
                /* uint16_t priority = readU16BE(data + rdataStart); */
                /* uint16_t weight   = readU16BE(data + rdataStart + 2); */
                foundPort = readU16BE(data + rdataStart + 4);
                size_t targetOffset = rdataStart + 6;
                foundHostname = parseDNSName(data, length, targetOffset);
                std::cout << "[Discovery] SRV: host=" << foundHostname
                          << " port=" << foundPort << std::endl;
            }
        }

        // ── A Record — IPv4 address ────────────────────────────────────
        else if (rtype == dns::TYPE_A) {
            if (rdlength == 4) {
                char ipBuf[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, data + rdataStart, ipBuf, sizeof(ipBuf));
                foundIP = ipBuf;
                std::cout << "[Discovery] A: " << recordName << " -> " << foundIP << std::endl;
            }
        }

        // Move past this record's data
        offset = rdataStart + rdlength;
    }

    // ── Construct discovered service ────────────────────────────────────────

    // Use IP from A record, or fall back to the packet's source IP
    std::string serviceIP = foundIP.empty() ? fromIP : foundIP;

    if (!foundInstanceName.empty()) {
        // Extract friendly name from instance name
        // Format: "ExternalDisplay-Alif's Mac._externaldisplay._tcp.local"
        std::string friendlyName = foundInstanceName;
        auto pos = friendlyName.find("._externaldisplay");
        if (pos != std::string::npos) {
            friendlyName = friendlyName.substr(0, pos);
        }

        DiscoveredService svc;
        svc.name = friendlyName;
        svc.host = foundHostname;
        svc.ipAddress = serviceIP;
        svc.port = foundPort > 0 ? foundPort : 9876; // Default to our TCP port
        svc.resolved = !serviceIP.empty();

        {
            std::lock_guard<std::mutex> lock(m_servicesMutex);
            auto it = m_services.find(friendlyName);
            bool isNew = (it == m_services.end());
            bool isUpdated = !isNew && (
                it->second.ipAddress != svc.ipAddress ||
                it->second.port != svc.port ||
                it->second.resolved != svc.resolved
            );

            m_services[friendlyName] = svc;

            if ((isNew || isUpdated) && m_discoveryCallback) {
                std::cout << "[Discovery] " << (isNew ? "New" : "Updated")
                          << " service: " << friendlyName
                          << " @ " << serviceIP << ":" << svc.port << std::endl;
                m_discoveryCallback(svc);
            }
        }
    }
}
