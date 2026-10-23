// =============================================================================
// AppLogicTests.cpp — Options, status texts, Mac choice, logging, replay window
// =============================================================================
// Portable (no Windows headers): builds with MSVC and with clang/gcc anywhere.
// =============================================================================

#include "App/Log.h"
#include "App/Options.h"
#include "App/StatusText.h"
#include "Security/ReplayWindow.h"
#include "Input/VideoViewport.h"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

static int gChecks = 0, gFailures = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFailures++; \
    std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

static void TestOptions() {
    std::printf("Options: parsing and warnings\n");
    Options o = ParseOptions("");
    CHECK(o.macAddress.empty() && o.gpu == GpuPreference::Default && o.renderer == RendererMode::Full);
    CHECK(o.benchSeconds == 0 && o.logLevel == LogLevel::Info && !o.showConsole && o.warnings.empty());

    o = ParseOptions("192.168.1.5 --gpu=high --bench=60 --log=trace --console --renderer=software");
    CHECK(o.macAddress == "192.168.1.5");
    CHECK(o.gpu == GpuPreference::HighPerformance);
    CHECK(o.benchSeconds == 60);
    CHECK(o.logLevel == LogLevel::Trace);
    CHECK(o.showConsole);
    CHECK(o.renderer == RendererMode::Software);

    o = ParseOptions("--verbose --renderer=compatible --test-pattern");
    CHECK(o.logLevel == LogLevel::Debug && o.renderer == RendererMode::Compatible && o.testPattern);

    o = ParseOptions("--bench=abc --log=loud --frobnicate 10.0.0.1 10.0.0.2");
    CHECK(o.benchSeconds == 0);
    CHECK(o.macAddress == "10.0.0.1");
    CHECK(o.warnings.size() == 4);   // bench, log, unknown option, extra argument
}

static void TestMacChoice() {
    std::printf("Mac choice: remembered Mac, single Mac after settling, else ask\n");
    std::vector<FoundMac> none;
    std::vector<FoundMac> one{ {"ExternalDisplay-Air", "10.0.0.2"} };
    std::vector<FoundMac> two{ {"ExternalDisplay-Air", "10.0.0.2"}, {"ExternalDisplay-Pro", "10.0.0.3"} };
    CHECK(AutoChooseMac(none, "", 5.0) == -1);
    CHECK(AutoChooseMac(one, "", 0.5) == -1);            // others may still answer
    CHECK(AutoChooseMac(one, "", 2.5) == 0);
    CHECK(AutoChooseMac(two, "", 10.0) == -1);           // user picks
    CHECK(AutoChooseMac(two, "ExternalDisplay-Pro", 0.0) == 1);   // remembered: immediately
    CHECK(AutoChooseMac(two, "ExternalDisplay-Gone", 10.0) == -1);
    CHECK(FriendlyMacName("ExternalDisplay-Alif's MacBook Air") == "Alif's MacBook Air");
    CHECK(FriendlyMacName("Other") == "Other");
}

static void TestStatusTexts() {
    std::printf("Status card: every phase has the right message\n");
    StatusInputs in;
    in.macName = "Air";
    in.address = "10.0.0.2";

    in.phase = AppPhase::Searching;
    StatusStrings s = BuildStatus(in);
    CHECK(Contains(s.title, "Looking"));
    CHECK(!Contains(s.body, "firewall"));
    in.secondsInPhase = 12;
    CHECK(Contains(BuildStatus(in).body, "firewall"));   // help after a while

    in.phase = AppPhase::Choosing;
    in.choices = { {"ExternalDisplay-Air", "10.0.0.2"}, {"ExternalDisplay-Pro", "10.0.0.3"} };
    s = BuildStatus(in);
    CHECK(Contains(s.body, "1    Air") && Contains(s.body, "2    Pro") && Contains(s.body, "10.0.0.3"));

    in.phase = AppPhase::Pairing;
    in.note = "That code does not match";
    s = BuildStatus(in);
    CHECK(Contains(s.title, "Pair with Air") && Contains(s.body, "6-digit") && Contains(s.body, "does not match"));

    in.note.clear();
    in.phase = AppPhase::WaitingForApproval;
    CHECK(Contains(BuildStatus(in).body, "Click Pair"));
    in.phase = AppPhase::WaitingForPicture;
    CHECK(Contains(BuildStatus(in).body, "Start Capture"));
    in.phase = AppPhase::Disconnected;
    CHECK(Contains(BuildStatus(in).body, "Esc"));
    in.phase = AppPhase::Streaming;
    s = BuildStatus(in);
    CHECK(s.title.empty() && s.footer.empty());

    in.phase = AppPhase::Connecting;
    in.rendererNote = "Graphics: compatible mode";
    CHECK(Contains(BuildStatus(in).body, "compatible mode"));
}

static void TestWiden() {
    std::printf("UTF-8 → wide: ASCII, accents, emoji, malformed input\n");
    CHECK(WidenUtf8("abc") == L"abc");
    CHECK(WidenUtf8("caf\xC3\xA9") == L"café");
    std::wstring emoji = WidenUtf8("\xF0\x9F\x98\x80");
    CHECK(emoji.size() == (sizeof(wchar_t) == 2 ? 2u : 1u));
    CHECK(WidenUtf8("\xC3") == L"?");            // truncated sequence
    CHECK(WidenUtf8("a\xFF" "b") == L"a?b");     // invalid lead byte
}

static void TestLogging() {
    std::printf("Logging: levels, file output, rate limiting\n");
    const std::string path = "applogic-test.log";
    std::remove(path.c_str());
    Log::SetConsoleEnabled(false);
    CHECK(Log::Init(path, LogLevel::Info));
    LOG_DEBUG("Test") << "hidden debug";
    LOG_INFO("Test") << "visible " << 42;
    LOGF_WARN("Test", "warn %d", 7);
    for (int i = 0; i < 5; i++) { LOG_EVERY_MS(60'000, LogLevel::Info, "Hot") << "once"; }
    LogLevel parsed;
    CHECK(Log::ParseLevel("warning", parsed) && parsed == LogLevel::Warning);
    CHECK(!Log::ParseLevel("loud", parsed));
    Log::Shutdown();

    std::ifstream file(path);
    std::stringstream text;
    text << file.rdbuf();
    std::string log = text.str();
    CHECK(Contains(log, "INFO  [Test] visible 42"));
    CHECK(Contains(log, "WARN  [Test] warn 7"));
    CHECK(!Contains(log, "hidden debug"));
    size_t onceCount = 0;
    for (size_t pos = 0; (pos = log.find("[Hot] once", pos)) != std::string::npos; pos++) onceCount++;
    CHECK(onceCount == 1);
    std::remove(path.c_str());
}

static void TestReplayWindow() {
    std::printf("Replay window: reordering accepted, duplicates and stale counters refused\n");
    ReplayWindow w;
    CHECK(w.accept(0));
    CHECK(!w.accept(0));
    CHECK(w.accept(2) && w.accept(1) && !w.accept(1));
    CHECK(w.accept(70));
    CHECK(w.accept(7));          // 63 behind
    CHECK(!w.accept(6));         // 64 behind
    CHECK(w.accept(10'000) && !w.accept(70));
}

static bool Near(float a, float b) { return a - b < 1e-4f && b - a < 1e-4f; }

static void TestVideoViewport() {
    std::printf("Mouse mapping: letterbox and pillarbox, edges, clamping\n");
    // 16:9 stream on a 16:10 panel (2560x1600): bars top and bottom
    VideoViewport v = ComputeVideoViewport(2560, 1600, 16.0f / 9.0f);
    CHECK(Near(v.x, 0) && Near(v.width, 2560) && Near(v.height, 1440) && Near(v.y, 80));
    float x, y;
    WindowToVideo(1280, 800, 2560, 1600, 16.0f / 9.0f, x, y);       // pixel-exact: last pixel = 1.0
    CHECK(Near(x, 1280.0f / 2559.0f) && Near(y, 720.0f / 1439.0f));
    WindowToVideo(0, 80, 2560, 1600, 16.0f / 9.0f, x, y);           // top-left of the picture
    CHECK(Near(x, 0) && Near(y, 0));
    WindowToVideo(2559, 1519, 2560, 1600, 16.0f / 9.0f, x, y);      // bottom-right pixel
    CHECK(Near(x, 1) && Near(y, 1));
    WindowToVideo(100, 20, 2560, 1600, 16.0f / 9.0f, x, y);         // in the top bar → edge
    CHECK(Near(y, 0));
    // Stretching over the whole window (the old mapping) would give 0.725 here
    WindowToVideo(0, 1160, 2560, 1600, 16.0f / 9.0f, x, y);
    CHECK(Near(y, 1080.0f / 1439.0f));

    // 16:10 stream on a 16:9 window: bars left and right
    v = ComputeVideoViewport(1920, 1080, 1.6f);
    CHECK(Near(v.y, 0) && Near(v.height, 1080) && Near(v.width, 1728) && Near(v.x, 96));
    WindowToVideo(50, 500, 1920, 1080, 1.6f, x, y);                 // in the left bar
    CHECK(Near(x, 0));

    // Unknown aspect: whole window
    WindowToVideo(959, 539, 1919, 1079, 0.0f, x, y);
    CHECK(Near(x, 0.5f) && Near(y, 0.5f));
    WindowToVideo(-5, 99999, 100, 100, 0.0f, x, y);
    CHECK(Near(x, 0) && Near(y, 1));
}

int main() {
    TestVideoViewport();
    TestOptions();
    TestMacChoice();
    TestStatusTexts();
    TestWiden();
    TestLogging();
    TestReplayWindow();
    std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
