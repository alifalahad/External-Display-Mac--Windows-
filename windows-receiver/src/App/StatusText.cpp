// =============================================================================
// StatusText.cpp — Status card wording and Mac selection rules
// =============================================================================

#include "App/StatusText.h"

#include <cstdint>
#include <cstdio>

namespace {

const char* kKeys = "F11 full screen  ·  F2 diagnostics  ·  F3 mouse & keyboard  ·  Esc quit";

std::string Seconds(double s) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", static_cast<int>(s));
    return buf;
}

std::string WithNote(std::string body, const std::string& note) {
    if (!note.empty()) body += "\n\n" + note;
    return body;
}

} // namespace

std::string FriendlyMacName(const std::string& serviceName) {
    static const std::string prefix = "ExternalDisplay-";
    if (serviceName.rfind(prefix, 0) == 0 && serviceName.size() > prefix.size()) {
        return serviceName.substr(prefix.size());
    }
    return serviceName.empty() ? "the Mac" : serviceName;
}

int AutoChooseMac(const std::vector<FoundMac>& found, const std::string& rememberedName,
                  double secondsSinceFirstFound, double settleSeconds) {
    if (!rememberedName.empty()) {
        for (size_t i = 0; i < found.size(); i++) {
            if (found[i].name == rememberedName) return static_cast<int>(i);
        }
    }
    if (found.size() == 1 && secondsSinceFirstFound >= settleSeconds) return 0;
    return -1;
}

StatusStrings BuildStatus(const StatusInputs& in) {
    StatusStrings s;
    s.footer = kKeys;
    const std::string mac = in.macName.empty() ? "the Mac" : in.macName;

    switch (in.phase) {
        case AppPhase::Searching:
            s.title = "Looking for your Mac…";
            s.body = "Open External Display on the Mac and click Start Capture.";
            if (in.secondsInPhase >= 10) {
                s.body += "\n\nStill nothing after " + Seconds(in.secondsInPhase) + " s? Check that both are on "
                          "the same network and that the firewall allows this app (mDNS, port 5353). "
                          "You can also start the receiver with the Mac's IP address.";
            }
            break;

        case AppPhase::Choosing: {
            s.title = "Choose a Mac";
            std::string list;
            for (size_t i = 0; i < in.choices.size() && i < 9; i++) {
                list += std::to_string(i + 1) + "    " + FriendlyMacName(in.choices[i].name) +
                        "    (" + in.choices[i].address + ")\n";
            }
            s.body = list + "\nPress the number of the Mac to use.";
            break;
        }

        case AppPhase::Connecting:
            s.title = "Connecting to " + mac + "…";
            s.body = in.address.empty() ? std::string() : in.address;
            if (in.secondsInPhase >= 6) {
                s.body += (s.body.empty() ? "" : "\n\n");
                s.body += "Make sure External Display is running on the Mac with Start Capture on.";
            }
            s.body = WithNote(s.body, in.note);
            break;

        case AppPhase::Pairing:
            s.title = "Pair with " + mac;
            s.body = WithNote("Type the 6-digit code shown in the Mac app.", in.note);
            break;

        case AppPhase::WaitingForApproval:
            s.title = "Approve on the Mac";
            s.body = WithNote("The code matched. Click Pair in the Mac app to finish.", in.note);
            break;

        case AppPhase::Securing:
            s.title = "Securing the connection…";
            s.body = WithNote("Authenticating with " + mac + ".", in.note);
            break;

        case AppPhase::WaitingForPicture:
            s.title = "Connected to " + mac;
            s.body = WithNote("Waiting for the picture. If nothing appears, click Start Capture in the Mac app.",
                              in.note);
            break;

        case AppPhase::Streaming:
            s.footer.clear();
            return s;   // the video is the UI

        case AppPhase::Disconnected:
            s.title = "Disconnected";
            s.body = WithNote("Press Esc to quit.", in.note);
            break;
    }
    if (!in.rendererNote.empty()) s.body += "\n\n" + in.rendererNote;
    return s;
}

std::wstring WidenUtf8(const std::string& utf8) {
    std::wstring out;
    out.reserve(utf8.size());
    for (size_t i = 0; i < utf8.size();) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp;
        size_t extra;
        if (c < 0x80)              { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        else { out.push_back(L'?'); i++; continue; }
        if (extra && i + extra >= utf8.size()) { out.push_back(L'?'); break; }   // truncated
        bool ok = true;
        for (size_t k = 1; k <= extra; k++) {
            unsigned char cc = static_cast<unsigned char>(utf8[i + k]);
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { out.push_back(L'?'); i++; continue; }
        i += extra + 1;
        if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
    }
    return out;
}
