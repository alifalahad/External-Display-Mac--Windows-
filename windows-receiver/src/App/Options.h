#pragma once
// =============================================================================
// Options.h — Command line of the receiver (portable, unit-tested)
// =============================================================================
//   ExternalDisplayReceiver.exe [mac-ip] [options]
//     --gpu=high|low|default                 GPU on hybrid laptops
//     --renderer=full|compatible|software    start lower in the fallback chain
//     --bench=SECONDS                        benchmark, then exit
//     --log=trace|debug|info|warning|error   log detail (receiver.log)
//     --verbose                              same as --log=debug
//     --console                              show a console window with the log
//     --test-pattern                         no network: animated test pattern
// =============================================================================

#include "App/Log.h"
#include "Renderer/RendererTypes.h"

#include <string>
#include <vector>

struct Options {
    std::string   macAddress;                   // empty = find the Mac on the network
    GpuPreference gpu = GpuPreference::Default;
    RendererMode  renderer = RendererMode::Full;
    int           benchSeconds = 0;
    LogLevel      logLevel = LogLevel::Info;
    bool          showConsole = false;
    bool          testPattern = false;
    std::vector<std::string> warnings;          // unknown / malformed options
};

Options ParseOptions(const std::string& commandLine);
