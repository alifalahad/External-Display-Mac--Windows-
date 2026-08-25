#pragma once
// =============================================================================
// TestPatternShader.h — Embedded HLSL shaders for the test pattern
// =============================================================================
// Uses the "fullscreen triangle" technique: 3 vertices from SV_VertexID,
// no vertex buffer needed. The pixel shader renders an animated test pattern
// with grid, bouncing circle, color bars, and a scan line.
// =============================================================================

namespace Shaders {

inline const char* kTestPatternHLSL = R"HLSL(

// ─── Vertex Shader ──────────────────────────────────────────────────────────
// Generates a fullscreen triangle from vertex ID (0, 1, 2).
// No vertex buffer or input layout required.

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VSOutput VSMain(uint id : SV_VertexID) {
    VSOutput output;
    // Produce UVs: (0,0), (2,0), (0,2) → covers the entire screen
    output.uv = float2((id << 1) & 2, id & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

// ─── Constants ──────────────────────────────────────────────────────────────

cbuffer Constants : register(b0) {
    float  gTime;
    float  gResX;
    float  gResY;
    float  gFrameCount;
};

// ─── Pixel Shader ───────────────────────────────────────────────────────────
// Animated test pattern: dark gradient, grid, bouncing circle, color bars, scan line.

float4 PSMain(VSOutput input) : SV_Target {
    float2 uv    = input.uv;
    float2 pixel = uv * float2(gResX, gResY);

    // ── Dark animated gradient background ──
    float3 col1 = float3(0.03, 0.03, 0.12);
    float3 col2 = float3(0.12, 0.03, 0.18);
    float3 color = lerp(col1, col2, uv.y + 0.08 * sin(gTime * 0.4 + uv.x * 3.14159));

    // ── Minor grid (every 80 pixels) ──
    float2 gridUV = frac(pixel / 80.0);
    float gridLine = (step(abs(gridUV.x - 0.5), 0.008) + step(abs(gridUV.y - 0.5), 0.008));
    gridLine = saturate(gridLine);
    color += float3(0.08, 0.08, 0.12) * gridLine;

    // ── Major grid (every 320 pixels) ──
    float2 majorUV = frac(pixel / 320.0);
    float majorLine = (step(abs(majorUV.x - 0.5), 0.004) + step(abs(majorUV.y - 0.5), 0.004));
    majorLine = saturate(majorLine);
    color += float3(0.15, 0.18, 0.25) * majorLine;

    // ── Center crosshair ──
    float chX = abs(uv.x - 0.5);
    float chY = abs(uv.y - 0.5);
    float crosshair = step(chX, 0.001) * step(chY, 0.04)
                    + step(chY, 0.001) * step(chX, 0.04);
    color += float3(0.4, 0.4, 0.4) * crosshair;

    // ── Bouncing circle (frame-smoothness indicator) ──
    float2 center = float2(0.5 + 0.35 * sin(gTime * 1.2),
                           0.45 + 0.25 * cos(gTime * 0.9));
    float dist = length(uv - center);
    float circle = smoothstep(0.055, 0.050, dist);
    float3 circleColor = float3(0.15, 0.55, 1.0);
    float glow = exp(-dist * 18.0) * 0.25;
    color = lerp(color, circleColor, circle);
    color += circleColor * glow;

    // ── Small tracking dot (faster movement for jitter detection) ──
    float2 dotCenter = float2(frac(gTime * 0.3), 0.1);
    float dotDist = length(uv - dotCenter);
    float dot = smoothstep(0.012, 0.008, dotDist);
    color = lerp(color, float3(1.0, 0.3, 0.1), dot);

    // ── SMPTE Color bars (bottom 8%) ──
    if (uv.y > 0.92) {
        float bi = floor(clamp(uv.x * 8.0, 0.0, 7.0));
        float3 barColor = float3(1, 1, 1); // default white

        if      (bi < 0.5) barColor = float3(1.0, 1.0, 1.0); // White
        else if (bi < 1.5) barColor = float3(1.0, 1.0, 0.0); // Yellow
        else if (bi < 2.5) barColor = float3(0.0, 1.0, 1.0); // Cyan
        else if (bi < 3.5) barColor = float3(0.0, 1.0, 0.0); // Green
        else if (bi < 4.5) barColor = float3(1.0, 0.0, 1.0); // Magenta
        else if (bi < 5.5) barColor = float3(1.0, 0.0, 0.0); // Red
        else if (bi < 6.5) barColor = float3(0.0, 0.0, 1.0); // Blue
        else               barColor = float3(0.0, 0.0, 0.0); // Black

        color = barColor * 0.85; // slightly dimmed for realism
    }

    // ── Horizontal scan line (shows time progression) ──
    float scanY = frac(gTime * 0.4);
    float scanDist = abs(uv.y - scanY);
    float scan = exp(-scanDist * 300.0) * 0.15;
    color += float3(0.0, scan, scan * 0.6);

    return float4(saturate(color), 1.0);
}

)HLSL";

} // namespace Shaders
