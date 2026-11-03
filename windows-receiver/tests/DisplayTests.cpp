// =============================================================================
// DisplayTests.cpp — Display path (docs/SMOOTHNESS.md, step 3)
// =============================================================================
// Which swap chains are tried in which order, when a present may tear, and
// (on Windows) per-monitor DPI awareness and the scaled dialog metrics.
// Portable except the last part: builds with MSVC and with clang/gcc anywhere.
// =============================================================================

#include "Renderer/RendererTypes.h"

#ifdef _WIN32
#include "App/Dpi.h"
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int gChecks = 0, gFailures = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFailures++; \
    std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static void TestSwapChainPlan() {
    std::printf("Swap chains: newest first, tearing only when the driver supports it\n");
    auto full = SwapChainPlan(RendererMode::Full, true);
    CHECK(full.size() == 5);
    CHECK(full[0].effect == SwapEffect::FlipDiscard && full[0].waitable && full[0].tearing);
    CHECK(full[1].effect == SwapEffect::FlipDiscard && full[1].waitable && !full[1].tearing);   // refused tearing: keep the waitable chain
    CHECK(full[2].effect == SwapEffect::FlipDiscard && !full[2].waitable);
    CHECK(full[3].effect == SwapEffect::FlipSequential);
    CHECK(full[4].effect == SwapEffect::Discard);

    auto noTearing = SwapChainPlan(RendererMode::Full, false);
    CHECK(noTearing.size() == 4 && noTearing[0].waitable && !noTearing[0].tearing);
    bool anyTearing = false;
    for (const auto& a : noTearing) anyTearing = anyTearing || a.tearing;
    CHECK(!anyTearing);

    // Compatible and software: no optional flags at all (what the second laptop may need)
    for (RendererMode mode : { RendererMode::Compatible, RendererMode::Software }) {
        auto plan = SwapChainPlan(mode, true);
        CHECK(plan.size() == 3);
        bool flags = false;
        for (const auto& a : plan) flags = flags || a.waitable || a.tearing;
        CHECK(!flags);
        CHECK(std::strcmp(plan[0].name, "flip-discard") == 0);
    }
}

static void TestPresentChoice() {
    std::printf("Present: on the refresh unless asked and allowed\n");
    PresentParams p = ChoosePresent(false, true);
    CHECK(p.syncInterval == 1 && !p.allowTearing);
    p = ChoosePresent(true, true);
    CHECK(p.syncInterval == 0 && p.allowTearing);
    p = ChoosePresent(true, false);              // asked, but this swap chain cannot tear
    CHECK(p.syncInterval == 1 && !p.allowTearing);
    p = ChoosePresent(false, false);
    CHECK(p.syncInterval == 1 && !p.allowTearing);
}

#ifdef _WIN32
static void TestDpi() {
    std::printf("DPI: per-monitor awareness, scaled lengths and fonts\n");
    const char* mode = EnablePerMonitorDpi();
    std::printf("  awareness: %s, system DPI %u\n", mode, DpiForWindow(nullptr));
    CHECK(std::strcmp(mode, "not DPI-aware") != 0);
    CHECK(DpiForWindow(nullptr) >= 72);

    CHECK(ScaleForDpi(100, 96) == 100);
    CHECK(ScaleForDpi(100, 144) == 150);          // 150 %
    CHECK(ScaleForDpi(360, 120) == 450);          // 125 %
    CHECK(ScaleForDpi(1280, 192) == 2560);        // 200 %

    // Fonts and frames grow with the DPI
    LOGFONTW at100 = MessageFontForDpi(96), at200 = MessageFontForDpi(192);
    CHECK(at100.lfHeight != 0 && std::abs(at200.lfHeight) > std::abs(at100.lfHeight));
    RECT r100 = { 0, 0, 360, 170 }, r200 = { 0, 0, 720, 340 };
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, exStyle = WS_EX_DLGMODALFRAME;
    AdjustWindowRectForDpi(r100, style, exStyle, 96);
    AdjustWindowRectForDpi(r200, style, exStyle, 192);
    CHECK(r100.right - r100.left > 360 && r100.bottom - r100.top > 170);   // frame and title bar added
    // At 200 % the frame is at least as large (Windows doubles it; Wine keeps it)
    CHECK((r200.bottom - r200.top) - 340 >= (r100.bottom - r100.top) - 170);
}
#endif

int main() {
    TestSwapChainPlan();
    TestPresentChoice();
#ifdef _WIN32
    TestDpi();
#endif
    std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
