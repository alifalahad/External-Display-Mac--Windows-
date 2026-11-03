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
//     --fec=on|off                           parity packets: rebuild lost packets (default on)
//     --pacing=on|off                        spread each frame's packets over ~1 ms (default on)
//     --simulate-loss=PERCENT[:BURST]        drop packets on purpose (tests loss repair)
//     --adapt=delay|tiers                    bitrate follows the delay (default) or loss/RTT tiers
//     --simulate-link=KBPS[:QUEUEMS]         a narrow link with a queue (tests --adapt)
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
    bool          parity = true;                // step 5
    bool          pacing = true;
    double        simulatedLossPercent = 0;     // 0 = off
    int           simulatedLossBurst = 1;
    bool          adaptToDelay = true;          // step 6
    double        simulatedLinkKbps = 0;        // 0 = off
    double        simulatedLinkQueueMs = 200;
    std::vector<std::string> warnings;          // unknown / malformed options
};

Options ParseOptions(const std::string& commandLine);
