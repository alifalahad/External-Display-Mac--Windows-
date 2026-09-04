// =============================================================================
// H264Decoder.cpp — Media Foundation H.264 decoder (NV12 output)
// =============================================================================

#include "H264Decoder.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <mfobjects.h>  // IMF2DBuffer

// Helper to safely release COM objects
template<typename T>
void SafeRelease(T** pp) {
    if (*pp) {
        (*pp)->Release();
        *pp = nullptr;
    }
}

H264Decoder::H264Decoder() {}

H264Decoder::~H264Decoder() {
    shutdown();
}

bool H264Decoder::initialize(uint32_t width, uint32_t height) {
    if (m_initialized) shutdown();

    m_width = width;
    m_height = height;

    // Initialize Media Foundation
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::cerr << "[Decoder] MFStartup failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    if (!createDecoder()) return false;
    if (!configureInput()) return false;
    if (!configureOutput()) return false;

    // Send stream start messages
    hr = m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    hr = m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    m_initialized = true;
    std::cout << "[Decoder] Initialized: " << width << "x" << height << std::endl;
    return true;
}

bool H264Decoder::createDecoder() {
    MFT_REGISTER_TYPE_INFO inputType = { MFMediaType_Video, MFVideoFormat_H264 };

    IMFActivate** activateArr = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFTEnumEx(
        MFT_CATEGORY_VIDEO_DECODER,
        MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
        MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
        &inputType, nullptr, &activateArr, &count
    );

    if (FAILED(hr) || count == 0) {
        std::cerr << "[Decoder] No H.264 decoder found" << std::endl;
        return false;
    }

    hr = activateArr[0]->ActivateObject(IID_PPV_ARGS(&m_decoder));
    for (UINT32 i = 0; i < count; i++) activateArr[i]->Release();
    CoTaskMemFree(activateArr);

    if (FAILED(hr)) {
        std::cerr << "[Decoder] Failed to activate decoder" << std::endl;
        return false;
    }

    // Set low latency mode
    IMFAttributes* attrs = nullptr;
    hr = m_decoder->GetAttributes(&attrs);
    if (SUCCEEDED(hr) && attrs) {
        attrs->SetUINT32(CODECAPI_AVLowLatencyMode, TRUE);
        attrs->Release();
    }

    return true;
}

bool H264Decoder::configureInput() {
    IMFMediaType* inputType = nullptr;
    HRESULT hr = MFCreateMediaType(&inputType);
    if (FAILED(hr)) return false;

    inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    MFSetAttributeSize(inputType, MF_MT_FRAME_SIZE, m_width, m_height);
    MFSetAttributeRatio(inputType, MF_MT_FRAME_RATE, 60, 1);
    MFSetAttributeRatio(inputType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    inputType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = m_decoder->SetInputType(0, inputType, 0);
    inputType->Release();

    if (FAILED(hr)) {
        std::cerr << "[Decoder] SetInputType failed: 0x" << std::hex << hr << std::endl;
        return false;
    }
    return true;
}

bool H264Decoder::configureOutput() {
    // Pick NV12 output (preferred for hardware decoders)
    IMFMediaType* outputType = nullptr;
    bool foundNV12 = false;

    for (DWORD i = 0; ; i++) {
        HRESULT hr = m_decoder->GetOutputAvailableType(0, i, &outputType);
        if (FAILED(hr)) break;

        GUID subtype;
        outputType->GetGUID(MF_MT_SUBTYPE, &subtype);

        if (subtype == MFVideoFormat_NV12) {
            hr = m_decoder->SetOutputType(0, outputType, 0);
            if (SUCCEEDED(hr)) foundNV12 = true;
            outputType->Release();
            if (foundNV12) break;
        } else {
            outputType->Release();
        }
    }

    if (!foundNV12) {
        std::cerr << "[Decoder] NV12 output not available" << std::endl;
        return false;
    }

    std::cout << "[Decoder] Output configured: NV12" << std::endl;
    return true;
}

bool H264Decoder::decode(const uint8_t* h264Data, size_t dataLen) {
    if (!m_initialized || !m_decoder) return false;

    auto startTime = std::chrono::high_resolution_clock::now();

    // Create input sample
    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;

    HRESULT hr = MFCreateMemoryBuffer((DWORD)dataLen, &buffer);
    if (FAILED(hr)) return false;

    BYTE* bufData = nullptr;
    hr = buffer->Lock(&bufData, nullptr, nullptr);
    if (SUCCEEDED(hr)) {
        memcpy(bufData, h264Data, dataLen);
        buffer->Unlock();
    }
    buffer->SetCurrentLength((DWORD)dataLen);

    hr = MFCreateSample(&sample);
    if (FAILED(hr)) { buffer->Release(); return false; }
    sample->AddBuffer(buffer);

    // Feed to decoder
    hr = m_decoder->ProcessInput(0, sample, 0);
    sample->Release();
    buffer->Release();

    if (FAILED(hr)) {
        if (hr == MF_E_NOTACCEPTING) {
            while (processOutput()) {}
        }
        return false;
    }

    // Pull output
    bool gotOutput = false;
    while (processOutput()) {
        gotOutput = true;
    }

    if (gotOutput) {
        auto endTime = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(endTime - startTime).count();
        m_stats.framesDecoded++;
        double alpha = 0.05;
        m_stats.avgDecodeLatencyMs = m_stats.avgDecodeLatencyMs * (1.0 - alpha) + ms * alpha;
    }

    return true;
}

bool H264Decoder::processOutput() {
    if (!m_decoder) return false;

    MFT_OUTPUT_DATA_BUFFER outputData{};
    DWORD status = 0;

    MFT_OUTPUT_STREAM_INFO streamInfo{};
    m_decoder->GetOutputStreamInfo(0, &streamInfo);

    IMFSample* outputSample = nullptr;
    IMFMediaBuffer* outputBuffer = nullptr;

    if (!(streamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES)) {
        HRESULT hr = MFCreateMemoryBuffer(streamInfo.cbSize, &outputBuffer);
        if (FAILED(hr)) return false;

        hr = MFCreateSample(&outputSample);
        if (FAILED(hr)) { outputBuffer->Release(); return false; }
        outputSample->AddBuffer(outputBuffer);
        outputData.pSample = outputSample;
    }

    HRESULT hr = m_decoder->ProcessOutput(0, 1, &outputData, &status);

    if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT) {
        SafeRelease(&outputSample);
        SafeRelease(&outputBuffer);
        return false;
    }

    if (hr == MF_E_TRANSFORM_STREAM_CHANGE) {
        configureOutput();
        SafeRelease(&outputSample);
        SafeRelease(&outputBuffer);
        return false;
    }

    if (FAILED(hr)) {
        SafeRelease(&outputSample);
        SafeRelease(&outputBuffer);
        return false;
    }

    IMFSample* resultSample = outputData.pSample;
    if (!resultSample) {
        SafeRelease(&outputSample);
        SafeRelease(&outputBuffer);
        return false;
    }

    // ── Deliver raw NV12 data to callback ───────────────────────────────────
    IMFMediaBuffer* resultBuffer = nullptr;
    hr = resultSample->ConvertToContiguousBuffer(&resultBuffer);
    if (SUCCEEDED(hr) && m_rawCallback) {
        // Try IMF2DBuffer for proper stride
        IMF2DBuffer* buffer2D = nullptr;
        HRESULT hr2D = resultBuffer->QueryInterface(IID_PPV_ARGS(&buffer2D));

        BYTE* nv12Data = nullptr;
        LONG nv12Stride = 0;
        bool locked2D = false;

        if (SUCCEEDED(hr2D) && buffer2D) {
            hr = buffer2D->Lock2D(&nv12Data, &nv12Stride);
            if (SUCCEEDED(hr)) locked2D = true;
            buffer2D->Release();
        }

        if (!locked2D) {
            DWORD nv12Len = 0;
            hr = resultBuffer->Lock(&nv12Data, nullptr, &nv12Len);
            // Guess stride: try 128-byte alignment, fallback to 16
            nv12Stride = ((LONG)m_width + 127) & ~127;
            uint32_t evenH = (m_height + 1) & ~1;
            if (nv12Len < (DWORD)(nv12Stride * evenH * 3 / 2)) {
                nv12Stride = ((LONG)m_width + 15) & ~15;
            }
        }

        if (nv12Data && nv12Stride > 0) {
            // Deliver raw NV12 — let the GPU do YUV→RGB conversion
            m_rawCallback(nv12Data, nv12Stride, m_width, m_height);
        }

        if (locked2D) {
            IMF2DBuffer* buf2D = nullptr;
            resultBuffer->QueryInterface(IID_PPV_ARGS(&buf2D));
            if (buf2D) { buf2D->Unlock2D(); buf2D->Release(); }
        } else {
            resultBuffer->Unlock();
        }

        resultBuffer->Release();
    }

    // Clean up
    if (outputData.pSample != outputSample) {
        outputData.pSample->Release();
    }
    SafeRelease(&outputSample);
    SafeRelease(&outputBuffer);

    return true;
}

void H264Decoder::flush() {
    if (m_decoder) {
        m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
    }
}

void H264Decoder::shutdown() {
    if (m_decoder) {
        m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_DRAIN, 0);
        while (processOutput()) {}
        SafeRelease(&m_decoder);
    }
    m_initialized = false;
    MFShutdown();
    std::cout << "[Decoder] Shutdown. Decoded " << m_stats.framesDecoded << " frames" << std::endl;
}
