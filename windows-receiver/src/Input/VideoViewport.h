#pragma once
// =============================================================================
// VideoViewport.h — Where the video sits in the window (portable, tested)
// =============================================================================
// The renderer letterboxes (bars top/bottom) or pillarboxes (bars left/right)
// when the window's shape differs from the stream's — a 16:9 stream on a
// 16:10 laptop panel, for example. Mouse positions must be mapped through the
// same rectangle, or clicks land beside their target on the Mac.
// =============================================================================

struct VideoViewport {
    float x = 0, y = 0, width = 0, height = 0;
};

/// videoAspect = width / height of the stream (≤ 0: fill the window)
inline VideoViewport ComputeVideoViewport(float windowW, float windowH, float videoAspect) {
    VideoViewport v{ 0, 0, windowW, windowH };
    if (videoAspect <= 0 || windowW <= 0 || windowH <= 0) return v;
    if (windowW / windowH > videoAspect) {          // wider window: pillarbox
        v.width = windowH * videoAspect;
        v.x = (windowW - v.width) / 2.0f;
    } else {                                        // taller window: letterbox
        v.height = windowW / videoAspect;
        v.y = (windowH - v.height) / 2.0f;
    }
    return v;
}

/// Window pixel → 0…1 across the video (clamped: the bars map to the edge)
inline void WindowToVideo(int wx, int wy, int windowW, int windowH, float videoAspect,
                          float& outX, float& outY) {
    VideoViewport v = ComputeVideoViewport((float)windowW, (float)windowH, videoAspect);
    if (v.width <= 1 || v.height <= 1) { outX = outY = 0; return; }
    float nx = ((float)wx - v.x) / (v.width - 1);
    float ny = ((float)wy - v.y) / (v.height - 1);
    outX = nx < 0 ? 0 : nx > 1 ? 1 : nx;
    outY = ny < 0 ? 0 : ny > 1 ? 1 : ny;
}
