// =============================================================================
// Options.cpp — Command line parsing
// =============================================================================

#include "App/Options.h"

#include <cstdlib>
#include <sstream>

Options ParseOptions(const std::string& commandLine) {
    Options o;
    std::istringstream args(commandLine);
    std::string arg;
    while (args >> arg) {
        if (arg == "--gpu=high")                 o.gpu = GpuPreference::HighPerformance;
        else if (arg == "--gpu=low")             o.gpu = GpuPreference::MinimumPower;
        else if (arg == "--gpu=default")         o.gpu = GpuPreference::Default;
        else if (arg == "--renderer=full")       o.renderer = RendererMode::Full;
        else if (arg == "--renderer=compatible") o.renderer = RendererMode::Compatible;
        else if (arg == "--renderer=software")   o.renderer = RendererMode::Software;
        else if (arg == "--verbose")             o.logLevel = LogLevel::Debug;
        else if (arg == "--console")             o.showConsole = true;
        else if (arg == "--test-pattern")        o.testPattern = true;
        else if (arg == "--cursor=local")        o.localCursor = true;
        else if (arg == "--cursor=video")        o.localCursor = false;
        else if (arg == "--present=vsync")       o.presentImmediately = false;
        else if (arg == "--present=immediate")   o.presentImmediately = true;
        else if (arg == "--fec=on")              o.parity = true;
        else if (arg == "--fec=off")             o.parity = false;
        else if (arg == "--pacing=on")           o.pacing = true;
        else if (arg == "--pacing=off")          o.pacing = false;
        else if (arg == "--adapt=delay")         o.adaptToDelay = true;
        else if (arg == "--adapt=tiers")         o.adaptToDelay = false;
        else if (arg.rfind("--log=", 0) == 0) {
            if (!Log::ParseLevel(arg.substr(6), o.logLevel)) o.warnings.push_back("Unknown log level: " + arg);
        } else if (arg.rfind("--simulate-loss=", 0) == 0) {
            // PERCENT or PERCENT:BURST, e.g. 2 or 2:4
            const std::string value = arg.substr(16);
            const size_t colon = value.find(':');
            o.simulatedLossPercent = std::atof(value.substr(0, colon).c_str());
            o.simulatedLossBurst = colon == std::string::npos ? 1 : std::atoi(value.c_str() + colon + 1);
            if (o.simulatedLossPercent <= 0 || o.simulatedLossPercent > 50 ||
                o.simulatedLossBurst < 1 || o.simulatedLossBurst > 32) {
                o.simulatedLossPercent = 0;
                o.simulatedLossBurst = 1;
                o.warnings.push_back("--simulate-loss needs PERCENT (0-50) and optionally :BURST (1-32), e.g. 2 or 2:4");
            }
        } else if (arg.rfind("--simulate-link=", 0) == 0) {
            // KBPS or KBPS:QUEUEMS, e.g. 8000 or 8000:100
            const std::string value = arg.substr(16);
            const size_t colon = value.find(':');
            o.simulatedLinkKbps = std::atof(value.substr(0, colon).c_str());
            o.simulatedLinkQueueMs = colon == std::string::npos ? 200 : std::atof(value.c_str() + colon + 1);
            if (o.simulatedLinkKbps < 500 || o.simulatedLinkKbps > 1'000'000 ||
                o.simulatedLinkQueueMs < 10 || o.simulatedLinkQueueMs > 2000) {
                o.simulatedLinkKbps = 0;
                o.simulatedLinkQueueMs = 200;
                o.warnings.push_back("--simulate-link needs KBPS (500-1000000) and optionally :QUEUEMS (10-2000), e.g. 8000");
            }
        } else if (arg.rfind("--bench=", 0) == 0) {
            o.benchSeconds = std::atoi(arg.c_str() + 8);
            if (o.benchSeconds <= 0) {
                o.benchSeconds = 0;
                o.warnings.push_back("--bench needs a duration in seconds, e.g. --bench=60");
            }
        } else if (arg.rfind("--", 0) == 0) {
            o.warnings.push_back("Ignoring unknown option: " + arg);
        } else if (o.macAddress.empty()) {
            o.macAddress = arg;
        } else {
            o.warnings.push_back("Ignoring extra argument: " + arg);
        }
    }
    return o;
}
