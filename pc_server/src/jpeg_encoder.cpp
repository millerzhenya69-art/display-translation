#include "jpeg_encoder.h"
#include <windows.h>
#include <wincodec.h>
#include <objbase.h>
#include <shlwapi.h>

static IWICImagingFactory* g_wicFactory = nullptr;

bool InitJpegEncoder() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) return false;

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&g_wicFactory));
    return SUCCEEDED(hr);
}

void ShutdownJpegEncoder() {
    if (g_wicFactory) {
        g_wicFactory->Release();
        g_wicFactory = nullptr;
    }
    CoUninitialize();
}

bool EncodeBgraToJpeg(const uint8_t* bgraData, int width, int height,
                      int quality, std::vector<uint8_t>& outJpeg) {
    if (!g_wicFactory) return false;

    IWICBitmap* bitmap = nullptr;
    HRESULT hr = g_wicFactory->CreateBitmapFromMemory(
        width, height,
        GUID_WICPixelFormat32bppBGRA,
        width * 4,
        static_cast<UINT>(width) * height * 4,
        const_cast<BYTE*>(bgraData),
        &bitmap);
    if (FAILED(hr)) return false;

    IWICFormatConverter* converter = nullptr;
    hr = g_wicFactory->CreateFormatConverter(&converter);
    if (FAILED(hr)) { bitmap->Release(); return false; }

    hr = converter->Initialize(bitmap, GUID_WICPixelFormat24bppBGR,
                                WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) { converter->Release(); bitmap->Release(); return false; }

    IStream* memStream = SHCreateMemStream(nullptr, 0);
    if (!memStream) { converter->Release(); bitmap->Release(); return false; }

    IWICBitmapEncoder* encoder = nullptr;
    hr = g_wicFactory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder);
    if (FAILED(hr)) {
        memStream->Release(); converter->Release(); bitmap->Release();
        return false;
    }

    hr = encoder->Initialize(memStream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) {
        encoder->Release(); memStream->Release(); converter->Release(); bitmap->Release();
        return false;
    }

    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* propBag = nullptr;
    hr = encoder->CreateNewFrame(&frame, &propBag);
    if (FAILED(hr)) {
        encoder->Release(); memStream->Release(); converter->Release(); bitmap->Release();
        return false;
    }

    PROPBAG2 option = {};
    option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
    VARIANT varValue;
    VariantInit(&varValue);
    varValue.vt = VT_R4;
    varValue.fltVal = static_cast<float>(quality) / 100.0f;
    propBag->Write(1, &option, &varValue);

    hr = frame->Initialize(propBag);
    propBag->Release();
    if (FAILED(hr)) {
        frame->Release(); encoder->Release(); memStream->Release();
        converter->Release(); bitmap->Release();
        return false;
    }

    frame->SetSize(width, height);
    WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
    frame->SetPixelFormat(&format);

    hr = frame->WriteSource(converter, nullptr);
    bool ok = SUCCEEDED(hr);

    if (ok) ok = SUCCEEDED(frame->Commit());
    if (ok) ok = SUCCEEDED(encoder->Commit());

    if (ok) {
        STATSTG stat;
        memStream->Stat(&stat, STATFLAG_NONAME);
        ULARGE_INTEGER size = stat.cbSize;
        outJpeg.resize(static_cast<size_t>(size.QuadPart));

        LARGE_INTEGER zero = {};
        memStream->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG bytesRead = 0;
        memStream->Read(outJpeg.data(), static_cast<ULONG>(outJpeg.size()), &bytesRead);
        outJpeg.resize(bytesRead);
    }

    frame->Release();
    encoder->Release();
    memStream->Release();
    converter->Release();
    bitmap->Release();

    return ok;
}
