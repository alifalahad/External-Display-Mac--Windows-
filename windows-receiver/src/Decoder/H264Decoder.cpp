// =============================================================================
// H264Decoder.cpp — Media Foundation H.264 decoder (NV12 output)
// =============================================================================

#include "H264Decoder.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <mfobjects.h>

template<typename T>
void SafeRelease(T** pp) {
    if (*pp) { (*pp)->Release(); *pp = nullptr; }
}

H264Decoder::H264Decoder() {}
H264Decoder::~H264Decoder() { shutdown(); }

bool H264Decoder::initialize(uint32_t width, uint32_t height) {
    if (m_initialized) shutdown();
    m_width = width;
    m_height = height;

    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) {
        std::cerr << "[Decoder] MFStartup failed: 0x" << std::hex << hr << std::endl;
        return false;
    }

    if (!createDecoder()) return false;
    if (!configureInput()) return false;
    if (!configureOutput()) return false;

    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    m_decoder->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

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
        &inputType, nullptr, &activateArr, &count);

    if (FAILED(hr) || count == 0) {
        std::cerr << "[Decoder] No H.264 decoder found" << std::endl;
        return false;
    }

    hr = activateArr[0]->ActivateObject(IID_PPV_ARGS(&m_decoder));
    for (UINT32 i = 0; i < count; i++) activateArr[i]->Release();
    CoTaskMemFree(activateArr);
    if (FAILED(hr)) return false;

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
    return SUCCEEDED(hr);
}

bool H264Decoder::configureOutput() {
    IMFMediaType* outputType = nullptr;
    bool found = false;
    for (DWORD i = 0; ; i++) {
        HRESULT hr = m_decoder->GetOutputAvailableType(0, i, &outputType);
        if (FAILED(hr)) break;
        GUID subtype;
        outputType->GetGUID(MF_MT_SUBTYPE, &subtype);
        if (subtype == MFVideoFormat_NV12) {
            hr = m_decoder->SetOutputType(0, outputType, 0);
            if (SUCCEEDED(hr)) found = true;
            outputType->Release();
            if (found) break;
        } else {
            outputType->Release();
        }
    }
    if (found) std::cout << "[Decoder] Output configured: NV12" << std::endl;
    return found;
}

bool H264Decoder::decode(const uint8_t* h264Data, size_t dataLen) {
    if (!m_initialized || !m_decoder) return false;
    auto startTime = std::chrono::high_resolution_clock::now();

    IMFSample* sample = nullptr;
    IMFMediaBuffer* buffer = nullptr;
    HRESULT hr = MFCreateMemoryBuffer((DWORD)dataLen, &buffer);
    if (FAILED(hr)) return false;

    BYTE* bufData = nullptr;
    buffer->Lock(&bufData, nullptr, nullptr);
    memcpy(bufData, h264Data, dataLen);
    buffer->Unlock();
    buffer->SetCurrentLength((DWORD)dataLen);

    MFCreateSample(&sample);
    sample->AddBuffer(buffer);

    hr = m_decoder->ProcessInput(0, sample, 0);
    sample->Release();
    buffer->Release();

    if (FAILED(hr)) {
        if (hr == MF_E_NOTACCEPTING) while (processOutput()) {}
        return false;
    }

    bool gotOutput = false;
    while (processOutput()) gotOutput = true;

    if (gotOutput) {
        double ms = std::chrono::duration<double, std::milli>(
            std::chrono::high_resolution_clock::now() - startTime).count();
        m_stats.framesDecoded++;
        m_stats.avgDecodeLatencyMs = m_stats.avgDecodeLatencyMs * 0.95 + ms * 0.05;
    }
    return true;
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
        MFCreateSample(&outputSample);
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

    // ── Extract NV12 planes ─────────────────────────────────────────────────
    IMFMediaBuffer* contiguous = nullptr;
    hr = resultSample->ConvertToContiguousBuffer(&contiguous);
    if (SUCCEEDED(hr) && m_rawCallback) {
        BYTE* data = nullptr;
        DWORD dataLen = 0;
        hr = contiguous->Lock(&data, nullptr, &dataLen);
        if (SUCCEEDED(hr) && data && dataLen > 0) {
            // Determine stride and aligned height from actual buffer size
            int stride = 0;
            uint32_t alignedH = 0;

            if (!DetermineNV12Layout(dataLen, m_width, m_height, stride, alignedH)) {
                // Fallback: assume stride = width, even-aligned height
                stride = (int)m_width;
                alignedH = (m_height + 1) & ~1;
            }

            // Y plane: offset 0, height rows of actual data
            // UV plane: offset stride * alignedH (after ALL Y rows including padding)
            const uint8_t* yData  = data;
            const uint8_t* uvData = data + (size_t)stride * alignedH;

            // Safety: verify UV data is within buffer
            size_t uvPlaneSize = (size_t)stride * (alignedH / 2);
            size_t uvEnd = (size_t)stride * alignedH + uvPlaneSize;
            if (uvEnd <= dataLen) {
                // Debug: log on first frame
                static bool logged = false;
                if (!logged) {
                    printf("[Decoder] NV12 layout: %u bytes, stride=%d, "
                           "alignedH=%u (actual %u), uvOffset=%zu\n",
                           dataLen, stride, alignedH, m_height,
                           (size_t)stride * alignedH);
                    fflush(stdout);
                    logged = true;
                }

                m_rawCallback(yData, uvData, stride, m_width, m_height);
            } else {
                printf("[Decoder] ERROR: UV plane out of bounds! "
                       "bufLen=%u, stride=%d, alignedH=%u, uvEnd=%zu\n",
                       dataLen, stride, alignedH, uvEnd);
                fflush(stdout);
            }

            contiguous->Unlock();
        }
        contiguous->Release();
    }

    if (outputData.pSample != outputSample) {
        outputData.pSample->Release();
    }
    SafeRelease(&outputSample);
    SafeRelease(&outputBuffer);
    return true;
}

void H264Decoder::flush() {
    if (m_decoder) m_decoder->ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH, 0);
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
