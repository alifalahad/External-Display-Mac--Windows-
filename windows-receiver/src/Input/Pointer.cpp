// =============================================================================
// Pointer.cpp — The Mac's pointer, drawn on this PC
// =============================================================================

#include "Input/Pointer.h"
#include "Input/VideoViewport.h"
#include "Network/Protocol.h"

#include <algorithm>
#include <cmath>
#include <cstring>

bool ParsePointerShape(const uint8_t* data, size_t len, PointerShape& out) {
    exdp::CursorShapeHeader h;
    if (!data || len < sizeof(h)) return false;
    std::memcpy(&h, data, sizeof(h));
    if (h.width < 1 || h.height < 1 || h.width > exdp::MAX_CURSOR_SIDE || h.height > exdp::MAX_CURSOR_SIDE) {
        return false;
    }
    if (h.hotspotX >= h.width || h.hotspotY >= h.height) return false;
    if (!std::isfinite(h.scale) || h.scale <= 0.01f || h.scale > 16.0f) return false;
    const size_t pixelBytes = static_cast<size_t>(h.width) * h.height * 4;
    if (len - sizeof(h) != pixelBytes) return false;

    out.id = h.shapeId;
    out.width = h.width;
    out.height = h.height;
    out.hotspotX = h.hotspotX;
    out.hotspotY = h.hotspotY;
    out.scale = h.scale;
    out.bgra.assign(data + sizeof(h), data + len);
    return true;
}

bool ParsePointerPosition(const uint8_t* data, size_t len, PointerPosition& out) {
    exdp::CursorPositionPayload p;
    if (!data || len < sizeof(p)) return false;
    std::memcpy(&p, data, sizeof(p));
    if (!std::isfinite(p.x) || !std::isfinite(p.y)) return false;
    out.x = std::min(std::max(p.x, 0.0f), 1.0f);
    out.y = std::min(std::max(p.y, 0.0f), 1.0f);
    out.visible = (p.flags & exdp::CURSOR_VISIBLE) != 0;
    return true;
}

std::vector<uint8_t> ResampleBgra(const std::vector<uint8_t>& src, int srcW, int srcH, int dstW, int dstH) {
    if (srcW < 1 || srcH < 1 || dstW < 1 || dstH < 1 ||
        src.size() != static_cast<size_t>(srcW) * srcH * 4) {
        return {};
    }
    if (srcW == dstW && srcH == dstH) return src;

    // Each destination pixel averages the source area it covers (box filter
    // with exact partial coverage)
    std::vector<uint8_t> out(static_cast<size_t>(dstW) * dstH * 4);
    const double sx = static_cast<double>(srcW) / dstW, sy = static_cast<double>(srcH) / dstH;
    for (int y = 0; y < dstH; y++) {
        const double y0 = y * sy, y1 = y0 + sy;
        for (int x = 0; x < dstW; x++) {
            const double x0 = x * sx, x1 = x0 + sx;
            double acc[4] = { 0, 0, 0, 0 }, area = 0;
            for (int iy = static_cast<int>(y0); iy < srcH && iy < y1; iy++) {
                const double wy = std::min(y1, iy + 1.0) - std::max(y0, static_cast<double>(iy));
                if (wy <= 0) continue;
                for (int ix = static_cast<int>(x0); ix < srcW && ix < x1; ix++) {
                    const double wx = std::min(x1, ix + 1.0) - std::max(x0, static_cast<double>(ix));
                    if (wx <= 0) continue;
                    const double w = wx * wy;
                    const uint8_t* p = &src[(static_cast<size_t>(iy) * srcW + ix) * 4];
                    for (int c = 0; c < 4; c++) acc[c] += p[c] * w;
                    area += w;
                }
            }
            uint8_t* o = &out[(static_cast<size_t>(y) * dstW + x) * 4];
            for (int c = 0; c < 4; c++) {
                o[c] = area > 0 ? static_cast<uint8_t>(std::lround(acc[c] / area)) : 0;
            }
        }
    }
    return out;
}

void UnpremultiplyBgra(std::vector<uint8_t>& pixels) {
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
        const int a = pixels[i + 3];
        for (int c = 0; c < 3; c++) {
            pixels[i + c] = a == 0 ? 0 : static_cast<uint8_t>(std::min(255, (pixels[i + c] * 255 + a / 2) / a));
        }
    }
}

PointerPlacement PlacePointer(const PointerShape& shape, float u, float v,
                              int windowW, int windowH, float videoAspect, uint32_t streamWidth) {
    PointerPlacement p;
    const VideoViewport vp = ComputeVideoViewport(static_cast<float>(windowW), static_cast<float>(windowH),
                                                  videoAspect);
    // Window pixels per bitmap pixel
    float k = shape.scale * (streamWidth > 0 ? vp.width / static_cast<float>(streamWidth) : 1.0f);
    k = std::min(std::max(k, 0.05f), 8.0f);
    constexpr int kMaxSide = 512;
    p.width = std::min(kMaxSide, std::max(1, static_cast<int>(std::lround(shape.width * k))));
    p.height = std::min(kMaxSide, std::max(1, static_cast<int>(std::lround(shape.height * k))));
    p.hotspotX = std::min(p.width - 1, std::max(0, static_cast<int>(std::lround(shape.hotspotX * k))));
    p.hotspotY = std::min(p.height - 1, std::max(0, static_cast<int>(std::lround(shape.hotspotY * k))));

    // Inverse of WindowToVideo: 1.0 is the last pixel
    const float px = vp.x + u * std::max(vp.width - 1, 0.0f);
    const float py = vp.y + v * std::max(vp.height - 1, 0.0f);
    p.x = static_cast<int>(std::lround(px)) - p.hotspotX;
    p.y = static_cast<int>(std::lround(py)) - p.hotspotY;
    return p;
}

void PointerModel::setForwarding(bool on) {
    // Just switched on: the Mac's pointer is where the Mac left it until our
    // mouse moves
    if (on && !forwarding_) macMovedLast_ = true;
    forwarding_ = on;
}

void PointerModel::onShape(PointerShape shape) {
    shape_ = std::move(shape);
    haveShape_ = true;
}

void PointerModel::onMacPosition(const PointerPosition& p) {
    mac_ = p;
    macMovedLast_ = true;
}

void PointerModel::onLocalMove(float u, float v) {
    if (!forwarding_) return;   // our mouse does not move the Mac's pointer
    macMovedLast_ = false;
    mac_ = PointerPosition{ u, v, true };   // the Mac's pointer follows ours
}

PointerView PointerModel::view() const {
    // No image yet: as before. An older Mac app never sends one and keeps
    // the pointer in the video.
    if (!local_ || !haveShape_) return forwarding_ ? PointerView::Hidden : PointerView::System;
    if (!forwarding_) return PointerView::System;
    return macMovedLast_ ? PointerView::Hidden : PointerView::MacShape;
}

bool PointerModel::drawPointer() const {
    return local_ && haveShape_ && mac_.visible && (!forwarding_ || macMovedLast_);
}
