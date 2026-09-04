// =============================================================================
// H264Decoder.h — Media Foundation H.264 hardware decoder
// =============================================================================
// Decodes H.264 Annex B bitstream using Media Foundation (DXVA).
// Outputs raw NV12 data for GPU-side YUV→RGB conversion.
//
// Pipeline: H.264 NALUs → MF H.264 Decoder (DXVA) → NV12 → GPU shader
// =============================================================================

#pragma once

#include <cstdint>
#include <vector>
#include <functional>
#include <mutex>

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

class H264Decoder {
public:
    /// Called when raw NV12 data is ready for GPU upload.
    /// Parameters: (yData, uvData, stride, width, height)
    /// yData  = pointer to Y plane (luma, full resolution)
    /// uvData = pointer to UV plane (chroma, half width/height, interleaved U0V0U1V1...)
    /// stride = bytes per row for both Y and UV planes
    using RawNV12Callback = std::function<void(
        const uint8_t* yData, const uint8_t* uvData,
        int stride, uint32_t width, uint32_t height)>;

    H264Decoder();
    ~H264Decoder();

    bool initialize(uint32_t width, uint32_t height);
    bool decode(const uint8_t* h264Data, size_t dataLen);
    void setRawNV12Callback(RawNV12Callback cb) { m_rawCallback = std::move(cb); }
    void flush();
    void shutdown();

    struct Stats {
        uint64_t framesDecoded = 0;
        uint64_t framesDropped = 0;
        double   avgDecodeLatencyMs = 0;
    };

    Stats getStats() const { return m_stats; }

private:
    bool createDecoder();
    bool configureInput();
    bool configureOutput();
    bool processOutput();

    IMFTransform* m_decoder = nullptr;
    uint32_t m_width = 0;
    uint32_t m_height = 0;
    bool m_initialized = false;

    RawNV12Callback m_rawCallback;
    Stats m_stats;
};
