#pragma once
// =============================================================================
// RendererTypes.h — Renderer choices shared with option parsing (portable)
// =============================================================================

/// Which GPU renders on hybrid (Intel + NVIDIA/AMD) laptops.
/// Default = the adapter Windows picks (normally the one wired to the panel).
enum class GpuPreference { Default, HighPerformance, MinimumPower };

/// How much of the GPU the renderer asks for. The receiver tries Full first and
/// falls back when a driver refuses a feature:
///   Full       — video-decode device, waitable swap chain (latency 1)
///   Compatible — plain hardware device, flip/blt fallback swap chains
///   Software   — WARP (CPU rasterizer): always works, slow, last resort
enum class RendererMode { Full, Compatible, Software };

inline const char* RendererModeName(RendererMode mode) {
    switch (mode) {
        case RendererMode::Full:       return "full";
        case RendererMode::Compatible: return "compatible";
        default:                       return "software (WARP)";
    }
}
