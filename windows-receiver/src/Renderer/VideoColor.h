#pragma once
// =============================================================================
// VideoColor.h — How the stream's YCbCr maps to RGB (portable, unit-tested)
// =============================================================================
// docs/SMOOTHNESS.md, step 4. The H.264 stream says in its SPS whether the
// samples use the full range (0–255) or the video range (16–235), and which
// matrix (BT.709 or BT.601). The shader used to assume full-range BT.709 for
// every stream — but a Mac app capturing BGRA sends video range, which then
// looked washed out (black at 6 %, white at 92 %). The receiver now reads the
// SPS and converts accordingly.
// =============================================================================

#include <cstdint>

struct VideoColor {
    bool fullRange = true;      // what the Mac app sends since step 4
    int  matrix = 1;            // H.264 matrix_coefficients: 1 BT.709, 5/6 BT.601, 2 unspecified

    bool operator==(const VideoColor& o) const { return fullRange == o.fullRange && matrix == o.matrix; }
    bool operator!=(const VideoColor& o) const { return !(*this == o); }
};

/// BT.601 or BT.709 — "unspecified" means BT.709 for HD, BT.601 below 720 lines
inline bool UsesBt601(const VideoColor& c, uint32_t height) {
    if (c.matrix == 5 || c.matrix == 6) return true;
    if (c.matrix == 1) return false;
    return height > 0 && height < 720;
}

inline const char* VideoColorName(const VideoColor& c, uint32_t height) {
    if (UsesBt601(c, height)) return c.fullRange ? "full-range BT.601" : "video-range BT.601";
    return c.fullRange ? "full-range BT.709" : "video-range BT.709";
}

/// What the NV12 shader does per pixel:
///   Y' = (Y − yOffset) × yScale,  C' = (C − cOffset) × cScale
///   R = Y' + crR·Cr',  G = Y' − cbG·Cb' − crG·Cr',  B = Y' + cbB·Cb'
struct VideoColorConstants {
    float yOffset, yScale, cOffset, cScale;
    float crR, cbG, crG, cbB;
};

inline VideoColorConstants ColorConstantsFor(const VideoColor& c, uint32_t height) {
    const bool bt601 = UsesBt601(c, height);
    const double kr = bt601 ? 0.299 : 0.2126, kb = bt601 ? 0.114 : 0.0722, kg = 1.0 - kr - kb;
    VideoColorConstants k{};
    k.yOffset = c.fullRange ? 0.0f : 16.0f / 255.0f;
    k.yScale  = c.fullRange ? 1.0f : 255.0f / 219.0f;
    k.cOffset = 128.0f / 255.0f;
    k.cScale  = c.fullRange ? 1.0f : 255.0f / 224.0f;
    k.crR = static_cast<float>(2.0 * (1.0 - kr));
    k.cbB = static_cast<float>(2.0 * (1.0 - kb));
    k.cbG = static_cast<float>(2.0 * kb * (1.0 - kb) / kg);
    k.crG = static_cast<float>(2.0 * kr * (1.0 - kr) / kg);
    return k;
}

/// The shader's arithmetic on the CPU (tests): 8-bit samples in, 0…1 out
inline void YuvToRgb(const VideoColorConstants& k, uint8_t y8, uint8_t cb8, uint8_t cr8, float rgb[3]) {
    const float y = (y8 / 255.0f - k.yOffset) * k.yScale;
    const float cb = (cb8 / 255.0f - k.cOffset) * k.cScale;
    const float cr = (cr8 / 255.0f - k.cOffset) * k.cScale;
    const float out[3] = { y + k.crR * cr, y - k.cbG * cb - k.crG * cr, y + k.cbB * cb };
    for (int i = 0; i < 3; i++) rgb[i] = out[i] < 0 ? 0 : out[i] > 1 ? 1 : out[i];
}
