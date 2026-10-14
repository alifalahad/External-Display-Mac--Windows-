// =============================================================================
// H264Decoder.cpp — Media Foundation H.264 decoder (D3D11 hardware or software)
// =============================================================================

#include "H264Decoder.h"
#include <iostream>
#include <cstdio>

H264Decoder::H264Decoder() {}
H264Decoder::~H264Decoder() { shutdown(); }

bool H264Decoder::initialize(uint32_t width, uint32_t height) {
    if (m_initialized || m_decoder) shutdown();
    m_width = width;
    m_height = height;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::cerr << "[Decoder] MFStartup failed: 0x" << std::hex << hr << std::dec << std::endl;
        return false;
    }
    m_mfStarted = true;

    if (!createDecoder()) return false;

    // Phase 17: decode on the GPU's video engine when the device allows it
    m_hardware = enableHardware();
    if (!configureInput() || !configureOutput()) {
        if (!m_hardware) return false;
        std::cout << "[Decoder] Hardware decoding rejected this stream — using software" << std::endl;
        m_decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        m_deviceManager.Reset();
        m_hardware = false;
        if (!configureInput() || !configureOutput()) return false;
    }

    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    m_initialized = true;
    std::cout << "[Decoder] Initialized: " << width << "x" << height
              << (m_hardware ? " (hardware, D3D11 video engine)" : " (SOFTWARE — CPU decoding)")
              << std::endl;
    return true;
}

bool H264Decoder::createDecoder() {
    // Synchronous MFTs only: an async (vendor) MFT would need the event model.
    // The Microsoft H.264 decoder is synchronous and uses DXVA when it gets a
    // D3D device manager.
    MFT_REGISTER_TYPE_INFO inputType = { MFMediaType_Video, MFVideoFormat_H264 };
    IMFActivate** activateArr = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputType, nullptr, &activateArr, &count);

    if (FAILED(hr) || count == 0) {
        std::cerr << "[Decoder] No H.264 decoder found" << std::endl;
        return false;
    }

    hr = activateArr[0]->ActivateObject(IID_PPV_ARGS(&m_decoder));
    for (UINT32 i = 0; i < count; i++) activateArr[i]->Release();
    CoTaskMemFree(activateArr);
    if (FAILED(hr)) return false;

    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(m_decoder->GetAttributes(&attrs)) && attrs) {
        // Output each picture as soon as it is decoded (no reorder buffering)
        attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
    }
    return true;
}

bool H264Decoder::enableHardware() {
    if (!m_device) return false;

    ComPtr<IMFAttributes> attrs;
    UINT32 d3d11Aware = 0;
    if (FAILED(m_decoder->GetAttributes(&attrs)) || !attrs ||
        FAILED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &d3d11Aware)) || !d3d11Aware) {
        std::cout << "[Decoder] Decoder is not D3D11-aware — software decoding" << std::endl;
        return false;
    }

    HRESULT hr = MFCreateDXGIDeviceManager(&m_resetToken, &m_deviceManager);
    if (SUCCEEDED(hr)) hr = m_deviceManager->ResetDevice(m_device.Get(), m_resetToken);
    if (SUCCEEDED(hr)) {
        hr = m_decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
                                       reinterpret_cast<ULONG_PTR>(m_deviceManager.Get()));
    }
    if (FAILED(hr)) {
        printf("[Decoder] GPU decoding unavailable (0x%08lX) — software decoding\n", (unsigned long)hr);
        m_deviceManager.Reset();
        return false;
    }
    return true;
}

bool H264Decoder::configureInput() {
    ComPtr<IMFMediaType> inputType;
    HRESULT hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) return false;

    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inputType.Get(), MF_MT_FRAME_SIZE, m_width, m_height);
    MFSetAttributeRatio(inputType.Get(), MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(inputType.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    return SUCCEEDED(m_decoder->SetInputType(0, inputType.Get(), 0));
}

bool H264Decoder::configureOutput() {
    for (DWORD i = 0; ; i++) {
        ComPtr<IMFMediaType> outputType;
        if (FAILED(m_decoder->GetOutputAvailableType(0, i, &outputType))) break;
        GUID subtype{};
        outputType->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype != MFVideoFormat_NV12) continue;
        if (FAILED(m_decoder->SetOutputType(0, outputType.Get(), 0))) continue;

        updateFrameSizeFrom(outputType.Get());
        MFT_OUTPUT_STREAM_INFO info{};
        m_decoder->GetOutputStreamInfo(0, &info);
        m_providesSamples = (info.dwFlags & (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES |
                                             MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
        std::cout << "[Decoder] Output configured: NV12" << std::endl;
        return true;
    }
    return false;
}

void H264Decoder::updateFrameSizeFrom(IMFMediaType* outputType) {
    // The real picture size comes from the bitstream (SPS). It can differ from
    // START_STREAM if the Mac restarted capture at another resolution.
    UINT32 w = 0, h = 0;
    MFVideoArea aperture{};
    bool hasAperture = SUCCEEDED(outputType->GetBlob(
        MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&aperture),
        sizeof(aperture), nullptr));
    if (hasAperture && aperture.Area.cx > 0 && aperture.Area.cy > 0) {
        w = (UINT32)aperture.Area.cx;
        h = (UINT32)aperture.Area.cy;
    } else if (FAILED(MFGetAttributeSize(outputType, MF_MT_FRAME_SIZE, &w, &h))) {
        return;
    } else if (w == m_width && h == ((m_height + 15) & ~15u)) {
        return;  // just macroblock padding (e.g. 1088 for 1080) — keep display size
    }

    if (w > 0 && h > 0 && (w != m_width || h != m_height)) {
        std::cout << "[Decoder] Stream size changed: " << m_width << "x" << m_height
                  << " -> " << w << "x" << h << std::endl;
        m_width = w;
        m_height = h;
    }
}

bool H264Decoder::decode(const uint8_t* h264Data, size_t dataLen, int64_t sampleTime) {
    if (!m_initialized || !m_decoder) return false;

    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateMemoryBuffer((DWORD)dataLen, &buffer);
    if (FAILED(hr)) return false;

    BYTE* bufData = nullptr;
    if (FAILED(buffer->Lock(&bufData, nullptr, nullptr))) return false;
    memcpy(bufData, h264Data, dataLen);
    buffer->Unlock();
    buffer->SetCurrentLength((DWORD)dataLen);

    ComPtr<IMFSample> sample;
    if (FAILED(MFCreateSample(&sample))) return false;
    sample->AddBuffer(buffer.Get());
    sample->SetSampleTime(sampleTime);
    sample->SetSampleDuration(166667);   // 1/60 s; informational only

    hr = m_decoder->ProcessInput(0, sample.Get(), 0);
    if (hr == MF_E_NOTACCEPTING) {
        // The decoder is full: collect its pictures, then resubmit. Dropping the
        // input here (as before) broke the reference chain until the next keyframe.
        drainOutput();
        m_stats.inputRetries++;
        hr = m_decoder->ProcessInput(0, sample.Get(), 0);
    }
    if (FAILED(hr)) return false;

    drainOutput();
    return true;
}

void H264Decoder::drainOutput() {
    if (!m_initialized || !m_decoder) return;
    // Bounded: a misbehaving decoder must not hang the UI thread
    for (int i = 0; i < 16 && processOutput(); i++) {}
}

// ── Helper: determine NV12 layout from buffer length ────────────────────────
// NV12 total bytes = stride × alignedHeight × 3/2
// Returns true if a valid layout was found.
static bool DetermineNV12Layout(DWORD bufLen, uint32_t width, uint32_t height,
                                 int& outStride, uint32_t& outAlignedH) {
    if (bufLen == 0 || bufLen % 3 != 0) return false;

    // stride × alignedH = bufLen × 2/3
    size_t product = (size_t)bufLen * 2 / 3;

    // Try common stride values (GPU alignment)
    int strideCandidates[] = {
        (int)width,                        // 1920 (no padding)
        ((int)width + 63)  & ~63,          // 1920 (64-aligned)
        ((int)width + 127) & ~127,         // 1920 (128-aligned)
        ((int)width + 255) & ~255,         // 2048 (256-aligned)
        ((int)width + 511) & ~511,         // 2048 (512-aligned)
    };

    for (int s : strideCandidates) {
        if (s < (int)width) continue;
        if (product % s != 0) continue;

        uint32_t h = (uint32_t)(product / s);
        if (h >= height && (h & 1) == 0 && h <= height + 256) {
            outStride = s;
            outAlignedH = h;
            return true;
        }
    }
    return false;
}

bool H264Decoder::processOutput() {
    MFT_OUTPUT_DATA_BUFFER outputData{};
    ComPtr<IMFSample> allocated;

    if (!m_providesSamples) {
        MFT_OUTPUT_STREAM_INFO info{};
        m_decoder->GetOutputStreamInfo(0, &info);
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(MFCreateMemoryBuffer(info.cbSize, &buffer)) || FAILED(MFCreateSample(&allocated))) {
            return false;
        }
        allocated->AddBuffer(buffer.Get());
        outputData.pSample = allocated.Get();
    }

    DWORD status = 0;
    HRESULT hr = m_decoder->ProcessOutput(0, 1, &outputData, &status);
    if (outputData.pEvents) outputData.pEvents->Release();

    // A sample the decoder allocated is ours to release
    ComPtr<IMFSample> result;
    if (outputData.pSample && outputData.pSample != allocated.Get()) {
        result.Attach(outputData.pSample);
    } else {
        result = allocated;
    }

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) return false;
    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        return configureOutput();   // new format negotiated; try again
    }
    if (FAILED(hr) || !result) return false;

    LONGLONG sampleTime = -1;
    if (FAILED(result->GetSampleTime(&sampleTime))) sampleTime = -1;

    // Hardware: the picture is a slice of the decoder's texture array
    ComPtr<IMFMediaBuffer> buffer;
    if (m_textureCallback && SUCCEEDED(result->GetBufferByIndex(0, &buffer))) {
        ComPtr<IMFDXGIBuffer> dxgiBuffer;
        ComPtr<ID3D11Texture2D> texture;
        UINT subresource = 0;
        if (SUCCEEDED(buffer.As(&dxgiBuffer)) &&
            SUCCEEDED(dxgiBuffer->GetResource(IID_PPV_ARGS(&texture))) &&
            SUCCEEDED(dxgiBuffer->GetSubresourceIndex(&subresource))) {
            m_stats.framesDecoded++;
            m_textureCallback(texture.Get(), subresource, m_width, m_height, sampleTime);
            return true;
        }
    }

    deliverSoftwareSample(result.Get(), sampleTime);
    return true;
}

void H264Decoder::deliverSoftwareSample(IMFSample* sample, int64_t sampleTime) {
    ComPtr<IMFMediaBuffer> contiguous;
    if (FAILED(sample->ConvertToContiguousBuffer(&contiguous)) || !m_rawCallback) return;

    BYTE* data = nullptr;
    DWORD dataLen = 0;
    if (FAILED(contiguous->Lock(&data, nullptr, &dataLen)) || !data || dataLen == 0) return;

    // Determine stride and aligned height from actual buffer size
    int stride = 0;
    uint32_t alignedH = 0;
    if (!DetermineNV12Layout(dataLen, m_width, m_height, stride, alignedH)) {
        // Fallback: assume stride = width, even-aligned height
        stride = (int)m_width;
        alignedH = (m_height + 1) & ~1;
    }

    // UV plane starts after ALL Y rows including padding
    size_t uvPlaneSize = (size_t)stride * (alignedH / 2);
    size_t uvEnd = (size_t)stride * alignedH + uvPlaneSize;
    if (uvEnd <= dataLen) {
        m_stats.framesDecoded++;
        m_rawCallback(data, data + (size_t)stride * alignedH, stride, m_width, m_height, sampleTime);
    } else {
        printf("[Decoder] ERROR: UV plane out of bounds! bufLen=%lu, stride=%d, alignedH=%u\n",
               (unsigned long)dataLen, stride, alignedH);
    }
    contiguous->Unlock();
}

void H264Decoder::shutdown() {
    if (m_decoder) {
        m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
        m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        if (m_hardware) m_decoder->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER, 0);
        m_decoder.Reset();
        std::cout << "[Decoder] Shutdown. Decoded " << m_stats.framesDecoded << " frames" << std::endl;
    }
    m_deviceManager.Reset();
    m_hardware = false;
    m_initialized = false;
    if (m_mfStarted) {
        MFShutdown();
        m_mfStarted = false;
    }
}
