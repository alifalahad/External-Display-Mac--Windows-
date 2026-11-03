// =============================================================================
// PointerTests.cpp — The Mac's pointer drawn on this PC (docs/SMOOTHNESS.md, step 1)
// =============================================================================
// Wire layout (the same bytes as the Mac's ProtocolTests), parsing of hostile
// input, resampling, placement through the letterbox, which pointer shows
// when, and (on Windows) building the Windows cursor.
// Portable except the last part: builds with MSVC and with clang/gcc anywhere.
// =============================================================================

#include "Input/Pointer.h"
#include "Network/Protocol.h"

#ifdef _WIN32
#include "Input/PointerCursor.h"
#endif

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

static int gChecks = 0, gFailures = 0;
#define CHECK(cond) do { gChecks++; if (!(cond)) { gFailures++; \
    std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::vector<uint8_t> FromHex(const std::string& hex) {
    std::vector<uint8_t> out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        out.push_back(static_cast<uint8_t>(std::stoul(hex.substr(i, 2), nullptr, 16)));
    }
    return out;
}

static std::string ToHex(const void* data, size_t len) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < len; i++) {
        uint8_t b = static_cast<const uint8_t*>(data)[i];
        s += digits[b >> 4];
        s += digits[b & 15];
    }
    return s;
}

// Same values and bytes as testPointerPayloadsMatchTheWindowsLayout (Mac)
static const char* kShapeHeaderHex = "0807060504030201" "0300" "0200" "0100" "0100" "0000003f" "00000000";
static const char* kPositionHex = "0000803e" "0000403f" "01000000";
static const char* kCapsHex = "01000000" "00000000";

static std::vector<uint8_t> ShapePayload(int w, int h, int hx, int hy, float scale, size_t pixelBytes) {
    exdp::CursorShapeHeader hdr;
    hdr.shapeId = 42;
    hdr.width = static_cast<uint16_t>(w);
    hdr.height = static_cast<uint16_t>(h);
    hdr.hotspotX = static_cast<uint16_t>(hx);
    hdr.hotspotY = static_cast<uint16_t>(hy);
    hdr.scale = scale;
    std::vector<uint8_t> out(sizeof(hdr) + pixelBytes, 0x80);
    std::memcpy(out.data(), &hdr, sizeof(hdr));
    return out;
}

static void TestWireLayout() {
    std::printf("Wire layout: identical to the Mac's bytes\n");
    std::vector<uint8_t> payload = FromHex(kShapeHeaderHex);
    const uint8_t pixels[3 * 2 * 4] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12,
                                        13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24 };
    payload.insert(payload.end(), pixels, pixels + sizeof(pixels));
    PointerShape shape;
    CHECK(ParsePointerShape(payload.data(), payload.size(), shape));
    CHECK(shape.id == 0x0102030405060708ULL);
    CHECK(shape.width == 3 && shape.height == 2);
    CHECK(shape.hotspotX == 1 && shape.hotspotY == 1);
    CHECK(shape.scale == 0.5f);
    CHECK(shape.bgra.size() == 24 && shape.bgra[0] == 1 && shape.bgra[23] == 24);

    std::vector<uint8_t> pos = FromHex(kPositionHex);
    PointerPosition p;
    CHECK(ParsePointerPosition(pos.data(), pos.size(), p));
    CHECK(p.x == 0.25f && p.y == 0.75f && p.visible);

    exdp::ReceiverCapsPayload caps;
    caps.flags = exdp::CAP_LOCAL_CURSOR;
    CHECK(ToHex(&caps, sizeof(caps)) == kCapsHex);
    CHECK(static_cast<int>(exdp::MessageType::ReceiverCaps) == 0x90);
    CHECK(static_cast<int>(exdp::MessageType::CursorShape) == 0x91);
    CHECK(static_cast<int>(exdp::MessageType::CursorPosition) == 0x92);
}

static void TestHostileInput() {
    std::printf("Parsing: malformed payloads are refused\n");
    PointerShape s;
    std::vector<uint8_t> ok = ShapePayload(4, 3, 1, 2, 0.5f, 4 * 3 * 4);
    CHECK(ParsePointerShape(ok.data(), ok.size(), s));
    CHECK(!ParsePointerShape(ok.data(), 23, s));                       // header cut
    CHECK(!ParsePointerShape(ok.data(), ok.size() - 1, s));            // pixels short
    std::vector<uint8_t> longer = ok;
    longer.push_back(0);
    CHECK(!ParsePointerShape(longer.data(), longer.size(), s));        // pixels long
    CHECK(!ParsePointerShape(nullptr, 100, s));

    auto bad = [&](int w, int h, int hx, int hy, float scale) {
        std::vector<uint8_t> p = ShapePayload(w, h, hx, hy, scale, static_cast<size_t>(w) * h * 4);
        return !ParsePointerShape(p.data(), p.size(), s);
    };
    CHECK(bad(0, 3, 0, 0, 0.5f));                                      // empty
    CHECK(bad(257, 1, 0, 0, 0.5f));                                    // larger than allowed
    CHECK(bad(4, 3, 4, 0, 0.5f));                                      // hot spot outside
    CHECK(bad(4, 3, 0, 3, 0.5f));
    CHECK(bad(4, 3, 0, 0, std::numeric_limits<float>::quiet_NaN()));
    CHECK(bad(4, 3, 0, 0, 0.0f));
    CHECK(bad(4, 3, 0, 0, 1000.0f));
    CHECK(!bad(256, 256, 255, 255, 1.0f));                             // the largest allowed

    exdp::CursorPositionPayload raw;
    raw.x = 1.5f;
    raw.y = -0.25f;
    raw.flags = 0;
    PointerPosition p;
    CHECK(ParsePointerPosition(reinterpret_cast<uint8_t*>(&raw), sizeof(raw), p));
    CHECK(p.x == 1.0f && p.y == 0.0f && !p.visible);                   // clamped; on another display
    raw.x = std::numeric_limits<float>::infinity();
    CHECK(!ParsePointerPosition(reinterpret_cast<uint8_t*>(&raw), sizeof(raw), p));
    CHECK(!ParsePointerPosition(reinterpret_cast<uint8_t*>(&raw), sizeof(raw) - 1, p));
}

static void TestResample() {
    std::printf("Resampling: exact area averages, premultiplied stays valid\n");
    // 4×2 → 2×1: each output pixel is the mean of a 2×2 block
    std::vector<uint8_t> src = {
        0, 0, 0, 0,        40, 40, 40, 40,      100, 0, 0, 200,   100, 0, 0, 200,
        80, 80, 80, 80,   120, 120, 120, 120,   100, 0, 0, 200,   100, 0, 0, 200,
    };
    std::vector<uint8_t> out = ResampleBgra(src, 4, 2, 2, 1);
    CHECK(out.size() == 8);
    CHECK(out[0] == 60 && out[3] == 60);                    // (0+40+80+120)/4
    CHECK(out[4] == 100 && out[5] == 0 && out[7] == 200);

    // 3 → 2 across: partial coverage (1, 0.5) and (0.5, 1)
    std::vector<uint8_t> row = { 0, 0, 0, 255,   90, 90, 90, 255,   180, 180, 180, 255 };
    out = ResampleBgra(row, 3, 1, 2, 1);
    CHECK(out.size() == 8 && out[0] == 30 && out[4] == 150 && out[3] == 255 && out[7] == 255);

    // Enlarging one pixel repeats it
    std::vector<uint8_t> one = { 10, 20, 30, 40 };
    out = ResampleBgra(one, 1, 1, 2, 2);
    CHECK(out.size() == 16 && out[12] == 10 && out[15] == 40);

    CHECK(ResampleBgra(src, 4, 2, 4, 2) == src);
    CHECK(ResampleBgra(src, 5, 2, 2, 1).empty());          // size does not match
    CHECK(ResampleBgra(src, 4, 2, 0, 1).empty());

    // Premultiplied input: no color above its alpha after any resample
    std::vector<uint8_t> noisy(17 * 13 * 4);
    uint32_t seed = 12345;
    for (size_t i = 0; i < noisy.size(); i += 4) {
        seed = seed * 1103515245u + 12345u;
        uint8_t a = static_cast<uint8_t>(seed >> 24);
        for (int c = 0; c < 3; c++) {
            seed = seed * 1103515245u + 12345u;
            noisy[i + c] = static_cast<uint8_t>(a ? (seed >> 24) % (a + 1) : 0);
        }
        noisy[i + 3] = a;
    }
    bool valid = true;
    for (auto [w, h] : { std::pair<int, int>{ 9, 7 }, { 5, 11 }, { 30, 20 }, { 1, 1 } }) {
        std::vector<uint8_t> r = ResampleBgra(noisy, 17, 13, w, h);
        for (size_t i = 0; i + 3 < r.size(); i += 4) {
            if (r[i] > r[i + 3] || r[i + 1] > r[i + 3] || r[i + 2] > r[i + 3]) valid = false;
        }
        if (r.size() != static_cast<size_t>(w) * h * 4) valid = false;
    }
    CHECK(valid);

    std::vector<uint8_t> px = { 64, 32, 0, 128,   7, 7, 7, 0,   10, 20, 30, 255 };
    UnpremultiplyBgra(px);
    CHECK(px[0] == 128 && px[1] == 64 && px[2] == 0 && px[3] == 128);
    CHECK(px[4] == 0 && px[7] == 0);                       // fully transparent
    CHECK(px[8] == 10 && px[9] == 20 && px[10] == 30);     // opaque unchanged
}

static void TestPlacement() {
    std::printf("Placement: size relative to the picture, hot spot, letterbox\n");
    PointerShape arrow;                                    // the Mac's arrow at 2×
    arrow.width = 56;
    arrow.height = 80;
    arrow.hotspotX = 9;
    arrow.hotspotY = 8;
    arrow.scale = 0.5f;                                    // 1920-point display streamed at 1920 px

    // Stream 1920 wide on a 1920×1080 window: the arrow's Mac size, 28×40
    PointerPlacement p = PlacePointer(arrow, 0.5f, 0.5f, 1920, 1080, 16.0f / 9.0f, 1920);
    CHECK(p.width == 28 && p.height == 40);
    CHECK(p.hotspotX == 5 && p.hotspotY == 4);             // 4.5 rounds up
    CHECK(p.x == 960 - 5 && p.y == 540 - 4);               // centre pixel 959.5 / 539.5

    // 16:10 laptop: picture letterboxed 2560×1440 at y = 80, 4/3 larger
    p = PlacePointer(arrow, 0.0f, 0.0f, 2560, 1600, 16.0f / 9.0f, 1920);
    CHECK(p.width == 37 && p.height == 53);
    CHECK(p.x == -p.hotspotX && p.y == 80 - p.hotspotY);   // top-left pixel of the picture
    p = PlacePointer(arrow, 1.0f, 1.0f, 2560, 1600, 16.0f / 9.0f, 1920);
    CHECK(p.x + p.hotspotX == 2559 && p.y + p.hotspotY == 1519);   // bottom-right pixel

    // Unknown stream size: one stream pixel per window pixel
    p = PlacePointer(arrow, 0.0f, 0.0f, 800, 600, 0.0f, 0);
    CHECK(p.width == 28 && p.height == 40);

    // Never absurdly large
    PointerShape huge = arrow;
    huge.width = huge.height = 256;
    huge.scale = 16.0f;
    p = PlacePointer(huge, 0.5f, 0.5f, 3840, 2160, 16.0f / 9.0f, 640);
    CHECK(p.width == 512 && p.height == 512);
}

static void TestWhichPointer() {
    std::printf("Pointer rules: Windows' pointer vs the drawn one\n");
    PointerShape shape;
    shape.width = shape.height = 4;
    shape.bgra.assign(64, 255);

    PointerModel m;
    // Pointer in the video (old Mac app, F4, or no shader)
    CHECK(m.view() == PointerView::System && !m.drawPointer());
    m.setForwarding(true);
    CHECK(m.view() == PointerView::Hidden && !m.drawPointer());
    m.setForwarding(false);

    // Drawn here, but no image yet (an older Mac app never sends one): as before
    m.setLocalPointer(true);
    m.onMacPosition({ 0.5f, 0.5f, true });
    CHECK(m.view() == PointerView::System && !m.drawPointer());
    m.setForwarding(true);
    CHECK(m.view() == PointerView::Hidden && !m.drawPointer());   // the Mac's pointer is in the video
    m.setForwarding(false);

    // F3 off: the PC's arrow plus the Mac's pointer where the Mac has it
    m.onShape(shape);
    CHECK(m.view() == PointerView::System && m.drawPointer());
    m.onLocalMove(0.1f, 0.1f);                             // our mouse does not move the Mac's
    CHECK(m.macPosition().x == 0.5f && m.drawPointer());
    m.onMacPosition({ 0, 0, false });                      // went to another Mac display
    CHECK(!m.drawPointer());
    m.onMacPosition({ 0.25f, 0.75f, true });

    // F3 on: until our mouse moves, the Mac's pointer stays where it is
    m.setForwarding(true);
    CHECK(m.view() == PointerView::Hidden && m.drawPointer());
    // Our mouse moves: Windows' pointer wears the Mac's image, nothing drawn
    m.onLocalMove(0.6f, 0.4f);
    CHECK(m.view() == PointerView::MacShape && !m.drawPointer());
    CHECK(m.macPosition().x == 0.6f && m.macPosition().y == 0.4f && m.macPosition().visible);
    // The Mac's trackpad takes over
    m.onMacPosition({ 0.3f, 0.3f, true });
    CHECK(m.view() == PointerView::Hidden && m.drawPointer() && m.macMovedLast());
    m.onLocalMove(0.31f, 0.3f);
    CHECK(m.view() == PointerView::MacShape);
    // Switched to the video (F4): back to the old behaviour at once
    m.setLocalPointer(false);
    CHECK(m.view() == PointerView::Hidden && !m.drawPointer());
    m.setLocalPointer(true);
    // F3 off and on again: the Mac's position counts until our mouse moves
    m.setForwarding(false);
    m.setForwarding(true);
    CHECK(m.view() == PointerView::Hidden && m.drawPointer());
}

#ifdef _WIN32
static void TestWindowsCursor() {
    std::printf("Windows cursor: size and hot spot\n");
    std::vector<uint8_t> pixels(32 * 24 * 4, 0);
    for (size_t i = 0; i < pixels.size(); i += 4) { pixels[i] = 200; pixels[i + 3] = 255; }
    HCURSOR cursor = CreatePointerCursor(pixels.data(), 32, 24, 3, 5);
    CHECK(cursor != nullptr);
    if (cursor) {
        ICONINFO info = {};
        CHECK(GetIconInfo(cursor, &info));
        CHECK(!info.fIcon && info.xHotspot == 3 && info.yHotspot == 5);
        BITMAP bm = {};
        if (info.hbmColor) GetObject(info.hbmColor, sizeof(bm), &bm);
        CHECK(bm.bmWidth == 32 && bm.bmHeight == 24);
        if (info.hbmColor) DeleteObject(info.hbmColor);
        if (info.hbmMask) DeleteObject(info.hbmMask);
        CHECK(DestroyIcon(cursor));
    }
    CHECK(CreatePointerCursor(nullptr, 32, 24, 0, 0) == nullptr);
    CHECK(CreatePointerCursor(pixels.data(), 0, 24, 0, 0) == nullptr);
}
#endif

int main() {
    TestWireLayout();
    TestHostileInput();
    TestResample();
    TestPlacement();
    TestWhichPointer();
#ifdef _WIN32
    TestWindowsCursor();
#endif
    std::printf("\n%d checks, %d failed\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
