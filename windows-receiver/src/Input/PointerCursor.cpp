// =============================================================================
// PointerCursor.cpp — A Windows cursor with the Mac's pointer image
// =============================================================================

#include "Input/PointerCursor.h"

#include <cstring>
#include <vector>

HCURSOR CreatePointerCursor(const uint8_t* bgra, int width, int height, int hotspotX, int hotspotY) {
    if (!bgra || width < 1 || height < 1) return nullptr;

    // 32-bit top-down DIB with an alpha channel
    BITMAPV5HEADER bi = {};
    bi.bV5Size = sizeof(bi);
    bi.bV5Width = width;
    bi.bV5Height = -height;
    bi.bV5Planes = 1;
    bi.bV5BitCount = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask = 0x00FF0000;
    bi.bV5GreenMask = 0x0000FF00;
    bi.bV5BlueMask = 0x000000FF;
    bi.bV5AlphaMask = 0xFF000000;

    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS,
                                     &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!color || !bits) {
        if (color) DeleteObject(color);
        return nullptr;
    }
    std::memcpy(bits, bgra, static_cast<size_t>(width) * height * 4);

    // A monochrome mask is required; with per-pixel alpha Windows ignores it.
    // Rows are word-aligned. All zero = "use the color image".
    std::vector<uint8_t> maskBits(static_cast<size_t>((width + 15) / 16) * 2 * height, 0);
    HBITMAP mask = CreateBitmap(width, height, 1, 1, maskBits.data());

    HCURSOR cursor = nullptr;
    if (mask) {
        ICONINFO info = {};
        info.fIcon = FALSE;   // a cursor: the hot spot counts
        info.xHotspot = static_cast<DWORD>(hotspotX);
        info.yHotspot = static_cast<DWORD>(hotspotY);
        info.hbmMask = mask;
        info.hbmColor = color;
        cursor = reinterpret_cast<HCURSOR>(CreateIconIndirect(&info));
        DeleteObject(mask);
    }
    DeleteObject(color);   // the cursor keeps its own copies
    return cursor;
}
