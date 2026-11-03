#pragma once
// =============================================================================
// RendererTypes.h — Renderer choices shared with option parsing (portable)
// =============================================================================

#include <vector>

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

// ── Swap chain and present (docs/SMOOTHNESS.md, step 3) ─────────────────────

enum class SwapEffect { FlipDiscard, FlipSequential, Discard };

/// One swap chain the renderer tries to create
struct SwapChainAttempt {
    SwapEffect effect;
    bool waitable;          // frame-latency waitable object (render right after the slot frees)
    bool tearing;           // DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING: may present without waiting
    const char* name;
};

/// Newest first. FLIP_DISCARD needs Windows 10, FLIP_SEQUENTIAL Windows 8; the
/// waitable object needs a flip model and Windows 8.1; tearing needs DXGI 1.5
/// and driver support (Windows 10 1607+). Compatible and software modes skip
/// the optional flags.
inline std::vector<SwapChainAttempt> SwapChainPlan(RendererMode mode, bool tearingSupported) {
    std::vector<SwapChainAttempt> plan;
    if (mode == RendererMode::Full) {
        if (tearingSupported) plan.push_back({ SwapEffect::FlipDiscard, true, true, "flip-discard, waitable, tearing allowed" });
        plan.push_back({ SwapEffect::FlipDiscard, true, false, "flip-discard, waitable" });
    }
    plan.push_back({ SwapEffect::FlipDiscard, false, false, "flip-discard" });
    plan.push_back({ SwapEffect::FlipSequential, false, false, "flip-sequential" });
    plan.push_back({ SwapEffect::Discard, false, false, "blt-discard" });
    return plan;
}

/// Present on the next refresh (sync interval 1), or at once when asked for
/// and the swap chain allows tearing: the picture can then change mid-refresh
/// (a visible "tear" while scrolling), up to one refresh sooner.
struct PresentParams {
    unsigned syncInterval;
    bool allowTearing;      // DXGI_PRESENT_ALLOW_TEARING
};

inline PresentParams ChoosePresent(bool immediateWanted, bool swapChainAllowsTearing) {
    if (immediateWanted && swapChainAllowsTearing) return { 0, true };
    return { 1, false };
}
