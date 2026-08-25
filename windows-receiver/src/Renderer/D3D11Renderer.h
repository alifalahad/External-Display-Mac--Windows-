#pragma once
// =============================================================================
// D3D11Renderer.h — Direct3D 11 GPU rendering pipeline
// =============================================================================
// Manages the D3D11 device, swap chain, shaders, and rendering.
// Renders an animated test pattern via GPU shader.
// Includes D2D1/DirectWrite overlay for on-screen statistics.
// =============================================================================

#include <d3d11.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <string>

class FrameStats;

class D3D11Renderer {
public:
    D3D11Renderer(HWND hwnd, int width, int height);
    ~D3D11Renderer() = default; // ComPtr handles cleanup

    // Render one frame: test pattern + optional stats overlay
    void Render(float time, float frameCount, const FrameStats* stats = nullptr);

    // Handle window resize
    void Resize(int width, int height);

    // Get GPU adapter name
    std::wstring GetAdapterName() const { return adapterName_; }

private:
    template<typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    void CreateDeviceAndSwapChain(HWND hwnd, int width, int height);
    void CreateRenderTarget();
    void CompileShaders();
    void CreateConstantBuffer();
    void CreateD2DResources();
    void ReleaseRenderTarget();

    // ── D3D11 core ──
    ComPtr<ID3D11Device>           device_;
    ComPtr<ID3D11DeviceContext>    context_;
    ComPtr<IDXGISwapChain1>        swapChain_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader>     vertexShader_;
    ComPtr<ID3D11PixelShader>      pixelShader_;
    ComPtr<ID3D11Buffer>           constantBuffer_;

    // ── D2D1/DirectWrite overlay ──
    ComPtr<ID2D1Factory1>          d2dFactory_;
    ComPtr<ID2D1Device>            d2dDevice_;
    ComPtr<ID2D1DeviceContext>     d2dContext_;
    ComPtr<IDWriteFactory>         dwriteFactory_;
    ComPtr<IDWriteTextFormat>      statsTextFormat_;
    ComPtr<IDWriteTextFormat>      titleTextFormat_;
    ComPtr<ID2D1SolidColorBrush>   textBrush_;
    ComPtr<ID2D1SolidColorBrush>   bgBrush_;
    ComPtr<ID2D1SolidColorBrush>   accentBrush_;

    HWND hwnd_ = nullptr;
    int  width_  = 0;
    int  height_ = 0;
    std::wstring adapterName_;

    // Shader constant buffer layout (must be 16-byte aligned)
    struct alignas(16) ShaderConstants {
        float time;
        float resolutionX;
        float resolutionY;
        float frameCount;
    };
};
