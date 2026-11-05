// =============================================================================
// DisplayTests.cpp — Display path (docs/SMOOTHNESS.md, step 3)
// =============================================================================
// Which swap chains are tried in which order, when a present may tear, and
// (on Windows) per-monitor DPI awareness and the scaled dialog metrics.
// Step 4: the stream's colours — SPS parsing (the Mac encoder's real bytes and
// constructed variants) and the shader's YCbCr→RGB arithmetic.
// Portable except the last part: builds with MSVC and with clang/gcc anywhere.
// =============================================================================

#include "Renderer/RendererTypes.h"
#include "Renderer/VideoColor.h"
#include "Decoder/H264Sps.h"

#ifdef _WIN32
#include "App/Dpi.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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

// ── Stream colours (docs/SMOOTHNESS.md, step 4) ─────────────────────────────

static std::vector<uint8_t> FromHex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    return out;
}

/// Writes SPS bits, then the stop bit, and inserts emulation-prevention bytes
class BitWriter {
public:
    void bits(uint32_t v, int n) { for (int i = n - 1; i >= 0; i--) bit((v >> i) & 1); }
    void ue(uint32_t v) {
        const uint64_t x = static_cast<uint64_t>(v) + 1;
        int len = 0;
        for (uint64_t t = x; t > 1; t >>= 1) len++;
        bits(0, len);
        for (int i = len; i >= 0; i--) bit(static_cast<uint32_t>((x >> i) & 1));
    }
    void se(int32_t v) { ue(v > 0 ? static_cast<uint32_t>(2 * v - 1) : static_cast<uint32_t>(-2 * v)); }
    std::vector<uint8_t> nal() {
        bit(1);
        while (n_ % 8) bit(0);
        std::vector<uint8_t> out{ 0x67 };   // nal_ref_idc 3, type 7 (SPS)
        int zeros = 0;
        for (uint8_t b : bytes_) {
            if (zeros >= 2 && b <= 3) { out.push_back(3); zeros = 0; }
            out.push_back(b);
            zeros = b == 0 ? zeros + 1 : 0;
        }
        return out;
    }
private:
    void bit(uint32_t b) {
        if (n_ % 8 == 0) bytes_.push_back(0);
        if (b) bytes_.back() |= static_cast<uint8_t>(0x80 >> (n_ % 8));
        n_++;
    }
    std::vector<uint8_t> bytes_;
    size_t n_ = 0;
};

struct SpsOptions {
    int profile = 100;
    bool scaling = false;
    int pocType = 0;
    bool frameMbsOnly = true;
    bool cropping = false;
    uint32_t widthMbsMinus1 = 119;
    bool vui = true;
    bool aspect = false, extendedSar = false, overscan = false;
    bool signalType = true, fullRange = true, colourDesc = true;
    int matrix = 1;
};

static std::vector<uint8_t> MakeSps(const SpsOptions& o) {
    BitWriter w;
    w.bits(static_cast<uint32_t>(o.profile), 8);
    w.bits(0, 8);
    w.bits(40, 8);
    w.ue(0);
    if (o.profile == 100) {
        w.ue(1);                                   // 4:2:0
        w.ue(0);
        w.ue(0);
        w.bits(0, 1);
        w.bits(o.scaling, 1);
        if (o.scaling) {
            for (int i = 0; i < 8; i++) {
                w.bits(i % 2 == 0, 1);
                if (i % 2 == 0) for (int j = 0; j < (i < 6 ? 16 : 64); j++) w.se(1);
            }
        }
    }
    w.ue(4);
    w.ue(static_cast<uint32_t>(o.pocType));
    if (o.pocType == 0) {
        w.ue(4);
    } else if (o.pocType == 1) {
        w.bits(0, 1);
        w.se(-3);
        w.se(2);
        w.ue(2);
        w.se(5);
        w.se(-7);
    }
    w.ue(1);
    w.bits(0, 1);
    w.ue(o.widthMbsMinus1);
    w.ue(67);
    w.bits(o.frameMbsOnly, 1);
    if (!o.frameMbsOnly) w.bits(0, 1);
    w.bits(1, 1);
    w.bits(o.cropping, 1);
    if (o.cropping) { w.ue(0); w.ue(0); w.ue(0); w.ue(4); }
    w.bits(o.vui, 1);
    if (o.vui) {
        w.bits(o.aspect, 1);
        if (o.aspect) {
            w.bits(o.extendedSar ? 255 : 1, 8);
            if (o.extendedSar) { w.bits(4, 16); w.bits(3, 16); }
        }
        w.bits(o.overscan, 1);
        if (o.overscan) w.bits(1, 1);
        w.bits(o.signalType, 1);
        if (o.signalType) {
            w.bits(5, 3);
            w.bits(o.fullRange, 1);
            w.bits(o.colourDesc, 1);
            if (o.colourDesc) { w.bits(1, 8); w.bits(1, 8); w.bits(static_cast<uint32_t>(o.matrix), 8); }
        }
        w.bits(0, 5);                              // no timing, HRD, pic_struct, restrictions
    }
    return w.nal();
}

static bool Parses(const SpsOptions& o, VideoColor& c) {
    std::vector<uint8_t> sps = MakeSps(o);
    return ParseSpsColor(sps.data(), sps.size(), c);
}

static void TestSps() {
    std::printf("SPS: colour description (the Mac encoder's real bytes and variants)\n");
    // Captured from VideoToolbox on an M2 (1080p, Constrained High), its own
    // reading in brackets: BGRA input [FullRangeVideo 0, no matrix] ...
    std::vector<uint8_t> bgra = FromHex("27640c28ac1314501e0089f966a0404040f0804258");
    // ... and 420f input with BT.709 tags [FullRangeVideo 1, ITU_R_709_2]
    std::vector<uint8_t> nv12 = FromHex("27640c28ac1314501e0089f966e0202020f0804258");
    VideoColor c;
    CHECK(ParseSpsColor(bgra.data(), bgra.size(), c) && !c.fullRange && c.matrix == 2);
    CHECK(ParseSpsColor(nv12.data(), nv12.size(), c) && c.fullRange && c.matrix == 1);
    CHECK(!ParseSpsColor(nv12.data(), 9, c));      // cut short
    CHECK(!ParseSpsColor(nv12.data() + 1, nv12.size() - 1, c));   // not an SPS

    // Found in an access unit: AUD and SEI first, 3- and 4-byte start codes
    std::vector<uint8_t> au = { 0, 0, 0, 1, 0x09, 0xF0, 0, 0, 1, 0x06, 0x05, 0x01, 0x80 };
    au.insert(au.end(), { 0, 0, 0, 1 });
    au.insert(au.end(), nv12.begin(), nv12.end());
    au.insert(au.end(), { 0, 0, 0, 1, 0x28, 0xEE, 0x3C, 0x80, 0, 0, 1, 0x25, 0x88, 0x84 });
    CHECK(FindSpsColor(au.data(), au.size(), c) && c.fullRange && c.matrix == 1);
    std::vector<uint8_t> pFrame = { 0, 0, 0, 1, 0x21, 0x9A, 0x00, 0x11 };
    CHECK(!FindSpsColor(pFrame.data(), pFrame.size(), c));

    // Constructed: every optional part the parser must step over
    SpsOptions o;
    CHECK(Parses(o, c) && c.fullRange && c.matrix == 1);
    o.fullRange = false;
    o.matrix = 6;
    CHECK(Parses(o, c) && !c.fullRange && c.matrix == 6);
    o = SpsOptions{};
    o.vui = false;                                 // standard's defaults
    CHECK(Parses(o, c) && !c.fullRange && c.matrix == 2);
    o = SpsOptions{};
    o.colourDesc = false;
    CHECK(Parses(o, c) && c.fullRange && c.matrix == 2);
    o = SpsOptions{};
    o.aspect = o.extendedSar = o.overscan = true;
    o.matrix = 5;
    CHECK(Parses(o, c) && c.fullRange && c.matrix == 5);
    o = SpsOptions{};
    o.scaling = true;
    o.pocType = 1;
    o.frameMbsOnly = false;
    o.cropping = true;
    CHECK(Parses(o, c) && c.fullRange && c.matrix == 1);
    o = SpsOptions{};
    o.profile = 66;                                // Baseline: no chroma/bit-depth fields
    o.pocType = 2;
    CHECK(Parses(o, c) && c.fullRange && c.matrix == 1);
    o = SpsOptions{};
    o.widthMbsMinus1 = 1u << 30;                   // 30 zero bits in a row: 00 00 03 inserted
    std::vector<uint8_t> escaped = MakeSps(o);
    bool hasEscape = false;
    for (size_t i = 2; i < escaped.size(); i++) hasEscape = hasEscape || (escaped[i - 2] == 0 && escaped[i - 1] == 0 && escaped[i] == 3);
    CHECK(hasEscape);
    CHECK(ParseSpsColor(escaped.data(), escaped.size(), c) && c.fullRange && c.matrix == 1);
}

static bool Near(float a, float b, float tol) { return std::fabs(a - b) <= tol; }

static void TestColourConversion() {
    std::printf("Colours: the shader's YCbCr→RGB for each range and matrix\n");
    const VideoColor full709{ true, 1 }, video709{ false, 1 }, video601{ false, 6 }, unspecified{ false, 2 };
    VideoColorConstants k = ColorConstantsFor(full709, 1080);
    CHECK(k.yOffset == 0 && k.yScale == 1 && k.cScale == 1);
    CHECK(Near(k.crR, 1.5748f, 1e-4f) && Near(k.cbG, 0.1873f, 1e-4f) && Near(k.crG, 0.4681f, 1e-4f) && Near(k.cbB, 1.8556f, 1e-4f));
    k = ColorConstantsFor(video601, 1080);
    CHECK(Near(k.crR, 1.402f, 1e-4f) && Near(k.cbB, 1.772f, 1e-4f) && Near(k.cbG, 0.3441f, 1e-4f) && Near(k.crG, 0.7141f, 1e-4f));
    CHECK(!UsesBt601(unspecified, 1080) && UsesBt601(unspecified, 480));   // HD convention
    CHECK(std::strcmp(VideoColorName(full709, 1080), "full-range BT.709") == 0);
    CHECK(std::strcmp(VideoColorName(unspecified, 480), "video-range BT.601") == 0);

    float rgb[3];
    // Black and white come out black and white in both ranges
    YuvToRgb(ColorConstantsFor(video709, 1080), 16, 128, 128, rgb);
    CHECK(Near(rgb[0], 0, 0.005f) && Near(rgb[1], 0, 0.005f) && Near(rgb[2], 0, 0.005f));
    YuvToRgb(ColorConstantsFor(video709, 1080), 235, 128, 128, rgb);
    CHECK(Near(rgb[0], 1, 0.005f) && Near(rgb[1], 1, 0.005f) && Near(rgb[2], 1, 0.005f));
    YuvToRgb(ColorConstantsFor(full709, 1080), 0, 128, 128, rgb);
    CHECK(Near(rgb[0], 0, 0.005f));
    YuvToRgb(ColorConstantsFor(full709, 1080), 255, 128, 128, rgb);
    CHECK(Near(rgb[2], 1, 0.005f));
    // The old bug: video-range samples shown as full range — washed out
    YuvToRgb(ColorConstantsFor(full709, 1080), 16, 128, 128, rgb);
    CHECK(Near(rgb[0], 16 / 255.0f, 0.005f));      // black at 6 %
    YuvToRgb(ColorConstantsFor(full709, 1080), 235, 128, 128, rgb);
    CHECK(Near(rgb[0], 235 / 255.0f, 0.005f));     // white at 92 %
    // Pure red in full-range BT.709: Y 54, Cb 99, Cr 255
    YuvToRgb(ColorConstantsFor(full709, 1080), 54, 99, 255, rgb);
    CHECK(Near(rgb[0], 1, 0.02f) && Near(rgb[1], 0, 0.02f) && Near(rgb[2], 0, 0.02f));
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

    // Step 7: the primary screen's refresh rate, told to the Mac (0: Windows cannot say)
    const UINT hz = RefreshHzForWindow(nullptr);
    std::printf("  primary screen: %u Hz\n", hz);
    CHECK(hz == 0 || (hz >= 24 && hz <= 500));
}
#endif

int main() {
    TestSwapChainPlan();
    TestPresentChoice();
    TestSps();
    TestColourConversion();
#ifdef _WIN32
    TestDpi();
#endif
    std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
