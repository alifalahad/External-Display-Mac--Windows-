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
        else if (arg.rfind("--log=", 0) == 0) {
            if (!Log::ParseLevel(arg.substr(6), o.logLevel)) o.warnings.push_back("Unknown log level: " + arg);
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
