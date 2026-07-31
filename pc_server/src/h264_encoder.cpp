#include "h264_encoder.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <codecapi.h>
#include <cstring>
#include <algorithm>

// ICodecAPI не объявлен в этой сборке MinGW-заголовков (codecapi.h содержит только GUID-ы
// свойств, но не сам интерфейс), поэтому объявляем вручную - бинарный макет COM
// интерфейса стандартен и задокументирован в Windows SDK (icodecapi.h).
struct ICodecAPI_Manual : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE IsSupported(const GUID* Api) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsModifiable(const GUID* Api) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParameterRange(const GUID* Api, VARIANT* ValueMin, VARIANT* ValueMax, VARIANT* SteppingDelta) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetParameterValues(const GUID* Api, VARIANT** Values, ULONG* ValuesCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDefaultValue(const GUID* Api, VARIANT* Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetValue(const GUID* Api, VARIANT* Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetValue(const GUID* Api, VARIANT* Value) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterForEvent(const GUID* Api, LONG_PTR userData) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterForEvent(const GUID* Api) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAllDefaults(void) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetValueWithNotify(const GUID* Api, VARIANT* Value, GUID** ChangedParam, ULONG* ChangedParamCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAllDefaultsWithNotify(GUID** ChangedParam, ULONG* ChangedParamCount) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAllSettings(IStream* pStream) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAllSettings(IStream* pStream) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetAllSettingsWithNotify(IStream* pStream, GUID** ChangedParam, ULONG* ChangedParamCount) = 0;
};
static const IID IID_ICodecAPI_Manual =
    { 0x901db4c7, 0x31ce, 0x41a2, { 0x85, 0xdc, 0x8f, 0xa0, 0xbf, 0x41, 0xb8, 0xda } };

H264Encoder::~H264Encoder() {
    Shutdown();
}

bool H264Encoder::Init(int width, int height, int fps, int bitrateBps) {
    // NV12 требует чётных размеров - подрезаем при необходимости.
    width_ = width & ~1;
    height_ = height & ~1;
    fps_ = fps;
    frameDuration_ = 10000000LL / std::max(1, fps_); // 100-нс единицы MF
    frameTime_ = 0;

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = MFStartup(MF_VERSION);
    if (FAILED(hr)) return false;

    IMFTransform* transform = nullptr;
    hr = CoCreateInstance(CLSID_MSH264EncoderMFT, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&transform));
    if (FAILED(hr)) return false;
    transform_ = transform;

    // Низкая задержка: отключает внутренний lookahead-буфер энкодера,
    // который иначе добавляет задержку в несколько кадров независимо от FPS.
    IMFAttributes* transformAttrs = nullptr;
    if (SUCCEEDED(transform->GetAttributes(&transformAttrs))) {
        transformAttrs->SetUINT32(MF_LOW_LATENCY, TRUE);
        transformAttrs->Release();
    }

    // --- Выходной тип (H.264) ---
    IMFMediaType* outType = nullptr;
    MFCreateMediaType(&outType);
    outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    outType->SetUINT32(MF_MT_AVG_BITRATE, bitrateBps);
    MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    outType->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_Base);

    hr = transform->SetOutputType(0, outType, 0);
    outType->Release();
    if (FAILED(hr)) return false;

    // --- Входной тип (NV12) ---
    IMFMediaType* inType = nullptr;
    MFCreateMediaType(&inType);
    inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, width_, height_);
    MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, fps_, 1);
    MFSetAttributeRatio(inType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);

    hr = transform->SetInputType(0, inType, 0);
    inType->Release();
    if (FAILED(hr)) return false;

    // Примечание: раньше тут был пропущен ICodecAPI из-за отсутствия в заголовках,
    // теперь он объявлен вручную выше - включаем реальное время и низкую задержку явно.
    ICodecAPI_Manual* codecApi = nullptr;
    if (SUCCEEDED(transform->QueryInterface(IID_ICodecAPI_Manual, (void**)&codecApi))) {
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BOOL;
        v.boolVal = VARIANT_TRUE;
        codecApi->SetValue(&CODECAPI_AVLowLatencyMode, &v);
        codecApi->SetValue(&CODECAPI_AVEncCommonRealTime, &v);

        VARIANT vZero;
        VariantInit(&vZero);
        vZero.vt = VT_UI4;
        vZero.ulVal = 0;
        codecApi->SetValue(&CODECAPI_AVEncMPVDefaultBPictureCount, &vZero); // 0 B-кадров

        codecApi->Release();
    }

    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);

    nv12Buffer_.resize(static_cast<size_t>(width_) * height_ * 3 / 2);
    started_ = true;
    return true;
}

void H264Encoder::ConvertBgraToNv12(const uint8_t* bgra, uint8_t* nv12) {
    uint8_t* yPlane = nv12;
    uint8_t* uvPlane = nv12 + static_cast<size_t>(width_) * height_;

    for (int y = 0; y < height_; ++y) {
        const uint8_t* row = bgra + static_cast<size_t>(y) * width_ * 4;
        uint8_t* yRow = yPlane + static_cast<size_t>(y) * width_;
        for (int x = 0; x < width_; ++x) {
            const uint8_t* px = row + x * 4;
            int b = px[0], g = px[1], r = px[2];
            int yy = ((66 * r + 129 * g + 25 * b + 128) >> 8) + 16;
            yRow[x] = static_cast<uint8_t>(std::clamp(yy, 0, 255));
        }
    }

    for (int y = 0; y < height_; y += 2) {
        uint8_t* uvRow = uvPlane + static_cast<size_t>(y / 2) * width_;
        for (int x = 0; x < width_; x += 2) {
            const uint8_t* p0 = bgra + static_cast<size_t>(y) * width_ * 4 + x * 4;
            int b = p0[0], g = p0[1], r = p0[2];
            int u = ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
            int v = ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
            uvRow[x] = static_cast<uint8_t>(std::clamp(u, 0, 255));
            uvRow[x + 1] = static_cast<uint8_t>(std::clamp(v, 0, 255));
        }
    }
}

bool H264Encoder::EncodeFrame(const uint8_t* bgraData, std::vector<uint8_t>& outNalUnits) {
    if (!started_) return false;
    IMFTransform* transform = static_cast<IMFTransform*>(transform_);

    ConvertBgraToNv12(bgraData, nv12Buffer_.data());

    IMFMediaBuffer* buffer = nullptr;
    MFCreateMemoryBuffer(static_cast<DWORD>(nv12Buffer_.size()), &buffer);

    BYTE* raw = nullptr;
    buffer->Lock(&raw, nullptr, nullptr);
    std::memcpy(raw, nv12Buffer_.data(), nv12Buffer_.size());
    buffer->Unlock();
    buffer->SetCurrentLength(static_cast<DWORD>(nv12Buffer_.size()));

    IMFSample* sample = nullptr;
    MFCreateSample(&sample);
    sample->AddBuffer(buffer);
    sample->SetSampleTime(frameTime_);
    sample->SetSampleDuration(frameDuration_);
    frameTime_ += frameDuration_;

    HRESULT hr = transform->ProcessInput(0, sample, 0);
    sample->Release();
    buffer->Release();

    if (FAILED(hr)) {
        return false;
    }

    bool gotAny = false;
    outNalUnits.clear();

    while (true) {
        MFT_OUTPUT_STREAM_INFO streamInfo = {};
        transform->GetOutputStreamInfo(0, &streamInfo);

        IMFMediaBuffer* outBuffer = nullptr;
        MFCreateMemoryBuffer(std::max<DWORD>(streamInfo.cbSize, 1 << 20), &outBuffer);

        IMFSample* outSample = nullptr;
        MFCreateSample(&outSample);
        outSample->AddBuffer(outBuffer);

        MFT_OUTPUT_DATA_BUFFER outputDataBuffer = {};
        outputDataBuffer.dwStreamID = 0;
        outputDataBuffer.pSample = outSample;

        DWORD status = 0;
        HRESULT hrOut = transform->ProcessOutput(0, 1, &outputDataBuffer, &status);

        if (hrOut == S_OK) {
            DWORD len = 0;
            BYTE* data = nullptr;
            outBuffer->Lock(&data, nullptr, nullptr);
            outBuffer->GetCurrentLength(&len);
            size_t oldSize = outNalUnits.size();
            outNalUnits.resize(oldSize + len);
            std::memcpy(outNalUnits.data() + oldSize, data, len);
            outBuffer->Unlock();
            gotAny = true;
        }

        outBuffer->Release();
        outSample->Release();

        if (hrOut != S_OK) break;
    }

    return gotAny;
}

void H264Encoder::Shutdown() {
    if (transform_) {
        IMFTransform* transform = static_cast<IMFTransform*>(transform_);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
        transform->Release();
        transform_ = nullptr;
        MFShutdown();
    }
    started_ = false;
}
