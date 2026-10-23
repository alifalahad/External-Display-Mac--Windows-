#pragma once
// =============================================================================
// StatusText.h — What the receiver window says when there is no picture
// =============================================================================
// Pure functions (portable, unit-tested): the app fills StatusInputs from its
// state, BuildStatus turns it into the card the renderer draws.
// =============================================================================

#include <string>
#include <vector>

enum class AppPhase {
    Searching,           // looking for Macs on the network
    Choosing,            // several Macs: press a number
    Connecting,          // TCP connect / reconnect
    Pairing,             // type the code shown on the Mac
    WaitingForApproval,  // code verified: click Pair on the Mac
    Securing,            // handshake with a paired Mac
    WaitingForPicture,   // authenticated, no video yet (or the Mac stopped)
    Streaming,           // video on screen: no card
    Disconnected,
};

struct FoundMac {
    std::string name;       // mDNS service name, e.g. "ExternalDisplay-Alif's MacBook Air"
    std::string address;
};

struct StatusInputs {
    AppPhase phase = AppPhase::Searching;
    std::string macName;                  // friendly name of the chosen Mac
    std::string address;
    std::vector<FoundMac> choices;        // Choosing
    double secondsInPhase = 0;
    std::string note;                     // last message worth showing (may be empty)
    std::string rendererNote;             // e.g. "software rendering" (may be empty)
};

struct StatusStrings {
    std::string title;
    std::string body;
    std::string footer;
};

/// Nothing to draw for AppPhase::Streaming (title empty)
StatusStrings BuildStatus(const StatusInputs& in);

/// "ExternalDisplay-Alif's MacBook Air" → "Alif's MacBook Air"
std::string FriendlyMacName(const std::string& serviceName);

/// Index of the Mac to connect to without asking, or -1 to let the user choose:
/// the remembered Mac if it is there, else the only one once the search has
/// had `settleSeconds` to find others.
int AutoChooseMac(const std::vector<FoundMac>& found, const std::string& rememberedName,
                  double secondsSinceFirstFound, double settleSeconds = 2.0);

/// UTF-8 → wide string for the renderer (UTF-16 on Windows)
std::wstring WidenUtf8(const std::string& utf8);
