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
//     --cursor=local|video                   pointer drawn here (default) or in the video
//     --present=vsync|immediate              present on the next refresh (default) or at once
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
    bool          localCursor = true;           // docs/SMOOTHNESS.md, step 1
    bool          presentImmediately = false;   // step 3: may tear, up to a refresh sooner
    std::vector<std::string> warnings;          // unknown / malformed options
};

Options ParseOptions(const std::string& commandLine);
