#pragma once
// =============================================================================
// Pointer.h — The Mac's pointer, drawn on this PC (portable, unit-tested)
// =============================================================================
// docs/SMOOTHNESS.md, step 1. When the receiver draws the pointer, the Mac
// captures without it and sends CURSOR_SHAPE / CURSOR_POSITION instead.
//
//   pointer here | F3 | who moved it last | Windows pointer | drawn pointer
//   no (video)   | off|                   | arrow           | —
//   no (video)   | on |                   | hidden          | —
//   yes          | off|                   | arrow           | at the Mac's position
//   yes          | on | the PC's mouse    | the Mac's image | —
//   yes          | on | the Mac           | hidden          | at the Mac's position
//
// With F3 on, Windows' own hardware pointer wears the Mac's image: it moves
// with no added latency while the Mac's pointer follows. Moves made on the
// Mac (trackpad) are drawn at the reported position. Until the Mac has sent
// an image (an older Mac app never does), everything stays as in row 1–2.
// =============================================================================

#include <cstddef>
#include <cstdint>
#include <vector>

struct PointerShape {
    uint64_t id = 0;
    int width = 0, height = 0;            // bitmap pixels
    int hotspotX = 0, hotspotY = 0;       // from the top-left
    float scale = 1;                      // stream pixels per bitmap pixel
    std::vector<uint8_t> bgra;            // premultiplied, top row first
};

struct PointerPosition {
    float x = 0, y = 0;                   // 0…1 across the video
    bool visible = false;                 // false: on another Mac display
};

/// CURSOR_SHAPE / CURSOR_POSITION payloads. False when malformed.
bool ParsePointerShape(const uint8_t* data, size_t len, PointerShape& out);
bool ParsePointerPosition(const uint8_t* data, size_t len, PointerPosition& out);

/// Area-weighted resample of premultiplied BGRA: shrinking the Mac's 2×
/// bitmap stays sharp, and premultiplied colors never bleed at the edges
std::vector<uint8_t> ResampleBgra(const std::vector<uint8_t>& src, int srcW, int srcH, int dstW, int dstH);

/// Premultiplied → straight alpha (what Windows cursors take)
void UnpremultiplyBgra(std::vector<uint8_t>& pixels);

/// The pointer image on screen, in window pixels
struct PointerPlacement {
    int x = 0, y = 0;                     // top-left of the image
    int width = 0, height = 0;
    int hotspotX = 0, hotspotY = 0;       // inside the image

    bool operator==(const PointerPlacement& o) const {
        return x == o.x && y == o.y && width == o.width && height == o.height &&
               hotspotX == o.hotspotX && hotspotY == o.hotspotY;
    }
    bool operator!=(const PointerPlacement& o) const { return !(*this == o); }
};

/// Hot spot at (u, v), 0…1 across the letterboxed video. The image keeps
/// the size it has on the Mac's screen relative to the picture.
/// streamWidth: stream pixels across (0 = unknown: one stream pixel per
/// window pixel).
PointerPlacement PlacePointer(const PointerShape& shape, float u, float v,
                              int windowW, int windowH, float videoAspect, uint32_t streamWidth);

/// What Windows shows over the video
enum class PointerView {
    System,      // Windows' normal arrow
    Hidden,      // nothing: the pointer is in the video, or we draw it
    MacShape,    // Windows' pointer with the Mac's image
};

class PointerModel {
public:
    /// The receiver draws the pointer (negotiated with the Mac, not switched off)
    void setLocalPointer(bool on) { local_ = on; }
    /// F3: mouse and keyboard go to the Mac
    void setForwarding(bool on);
    void onShape(PointerShape shape);
    /// The pointer moved on the Mac (the Mac never echoes our own moves)
    void onMacPosition(const PointerPosition& p);
    /// Our mouse moved over the video (u, v: 0…1)
    void onLocalMove(float u, float v);

    PointerView view() const;
    /// Draw the Mac's image ourselves at macPosition()
    bool drawPointer() const;
    const PointerShape* shape() const { return haveShape_ ? &shape_ : nullptr; }
    PointerPosition macPosition() const { return mac_; }
    bool macMovedLast() const { return macMovedLast_; }

private:
    bool local_ = false;
    bool forwarding_ = false;
    bool haveShape_ = false;
    bool macMovedLast_ = true;            // until our mouse moves, the Mac's position counts
    PointerShape shape_;
    PointerPosition mac_;
};
