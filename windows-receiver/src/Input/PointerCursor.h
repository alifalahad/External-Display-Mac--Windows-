#pragma once
// =============================================================================
// PointerCursor.h — A Windows cursor with the Mac's pointer image
// =============================================================================
// Windows moves its hardware pointer itself, at the mouse's rate: wearing the
// Mac's image, it feels exactly like a local pointer (docs/SMOOTHNESS.md,
// step 1).
// =============================================================================

#include <Windows.h>
#include <cstdint>

/// bgra: straight (not premultiplied) alpha, top row first, width × height × 4.
/// Returns nullptr on failure. Free with DestroyIcon once no longer set.
HCURSOR CreatePointerCursor(const uint8_t* bgra, int width, int height, int hotspotX, int hotspotY);
