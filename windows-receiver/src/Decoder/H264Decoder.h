// =============================================================================
// H264Decoder.h — Media Foundation H.264 decoder (D3D11 hardware or software)
// =============================================================================
// Hardware path (Phase 17): the Microsoft H.264 decoder MFT is given the
// renderer's D3D11 device (IMFDXGIDeviceManager) and decodes on the GPU's
// video engine. Pictures stay in GPU memory:
//
//   H.264 → MFT (DXVA) → NV12 texture → callback → GPU copy → renderer
//
// Software path (fallback when the GPU/driver can't): NV12 in system memory,
// uploaded by the renderer.
//
// Not thread-safe: create, decode and drain on one thread (the UI thread).
// =============================================================================

#pragma once

#include <cstdint>
#include <functional>

#include <d3d11.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <wrl/client.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

class H264Decoder {
public:
    /// Software output: NV12 planes in system memory, valid during the call.
    /// stride = bytes per row of both planes; sampleTime = value given to decode()
    using RawNV12Callback = std::function<void(
        const uint8_t* yData, const uint8_t* uvData,
        int stride, uint32_t width, uint32_t height, int64_t sampleTime)>;

    /// Hardware output: an NV12 texture (array slice `subresource`) owned by the
    /// decoder — copy it on the GPU before returning
    using TextureCallback = std::function<void(
        ID3D11Texture2D* texture, UINT subresource,
        uint32_t width, uint32_t height, int64_t sampleTime)>;

    H264Decoder();
    ~H264Decoder();
    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    /// Decode on this device's GPU (call before initialize). nullptr = software.
    /// The device needs D3D11_CREATE_DEVICE_VIDEO_SUPPORT and multithread protection.
    void setD3D11Device(ID3D11Device* device) { m_device = device; }

    bool initialize(uint32_t width, uint32_t height);
    void shutdown();

    /// Submit one Annex B access unit; pictures that are ready are delivered
    /// through the callbacks before this returns. sampleTime is in 100 ns units.
    bool decode(const uint8_t* h264Data, size_t dataLen, int64_t sampleTime);

    /// Deliver pictures the decoder finished since the last call. Cheap when
    /// there are none; call it when the decoder still holds submitted frames.
    void drainOutput();

    void setRawNV12Callback(RawNV12Callback cb) { m_rawCallback = std::move(cb); }
    void setTextureCallback(TextureCallback cb) { m_textureCallback = std::move(cb); }

    bool isHardwareAccelerated() const { return m_hardware; }

    struct Stats {
        uint64_t framesDecoded = 0;
        uint64_t inputRetries = 0;   // MF_E_NOTACCEPTING: output drained, input resubmitted
    };
    Stats getStats() const { return m_stats; }

private:
    template<typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool createDecoder();
    bool enableHardware();
    bool configureInput();
    bool configureOutput();
    bool processOutput();
    void deliverSoftwareSample(IMFSample* sample, int64_t sampleTime);
    void updateFrameSizeFrom(IMFMediaType* outputType);

    ComPtr<IMFTransform>         m_decoder;
    ComPtr<ID3D11Device>         m_device;
    ComPtr<IMFDXGIDeviceManager> m_deviceManager;
    UINT     m_resetToken = 0;
    bool     m_hardware = false;
    bool     m_mfStarted = false;
    bool     m_providesSamples = false;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool     m_initialized = false;

    RawNV12Callback m_rawCallback;
    TextureCallback m_textureCallback;
    Stats m_stats;
};
