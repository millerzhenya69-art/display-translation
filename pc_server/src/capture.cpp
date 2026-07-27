#include "capture.h"
#include <dxgi1_2.h>
#include <cstring>
#include <algorithm>
#include <iostream>

// Важно: используем только адаптер 0 (основной GPU) - именно его использует
// D3D11CreateDevice в ScreenCapture::Init по умолчанию, поэтому индексы должны совпадать.
static IDXGIAdapter1* GetPrimaryAdapter(IDXGIFactory1* factory) {
    IDXGIAdapter1* adapter = nullptr;
    factory->EnumAdapters1(0, &adapter);
    return adapter;
}

void ListAvailableOutputs() {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
        std::cerr << "Не удалось получить список мониторов\n";
        return;
    }

    std::cout << "Доступные мониторы (индекс: имя, разрешение):\n";

    IDXGIAdapter1* adapter = GetPrimaryAdapter(factory);
    if (adapter) {
        IDXGIOutput* output = nullptr;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc))) {
                int w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
                int h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

                // Конвертируем широкую строку в UTF-8, чтобы не смешивать cout/wcout на одном потоке (UB)
                char nameUtf8[256] = {};
                WideCharToMultiByte(CP_UTF8, 0, desc.DeviceName, -1, nameUtf8, sizeof(nameUtf8), nullptr, nullptr);

                std::cout << "  [" << o << "] " << nameUtf8
                          << " - " << w << "x" << h
                          << (desc.AttachedToDesktop ? "" : " (отключен)") << "\n";
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    std::cout << "(индекс выше - это значение для monitor_index в config.txt)\n\n";
}

bool GetOutputResolution(int outputIndex, int& outW, int& outH) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) return false;

    bool found = false;
    IDXGIAdapter1* adapter = GetPrimaryAdapter(factory);
    if (adapter) {
        IDXGIOutput* output = nullptr;
        if (adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc))) {
                outW = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
                outH = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
                found = true;
            }
            output->Release();
        }
        adapter->Release();
    }
    factory->Release();
    return found;
}

ScreenCapture::~ScreenCapture() {
    Shutdown();
}

bool ScreenCapture::Init(int outputIndex) {
    HRESULT hr;

    D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 };
    D3D_FEATURE_LEVEL obtained;

    hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        0,
        levels, ARRAYSIZE(levels),
        D3D11_SDK_VERSION,
        &device_,
        &obtained,
        &context_
    );
    if (FAILED(hr)) return false;

    IDXGIDevice* dxgiDevice = nullptr;
    hr = device_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDevice);
    if (FAILED(hr)) return false;

    IDXGIAdapter* adapter = nullptr;
    hr = dxgiDevice->GetAdapter(&adapter);
    dxgiDevice->Release();
    if (FAILED(hr)) return false;

    IDXGIOutput* output = nullptr;
    hr = adapter->EnumOutputs(outputIndex, &output);
    adapter->Release();
    if (FAILED(hr)) return false;

    IDXGIOutput1* output1 = nullptr;
    hr = output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1);
    output->Release();
    if (FAILED(hr)) return false;

    hr = output1->DuplicateOutput(device_, &duplication_);
    output1->Release();
    if (FAILED(hr)) return false;

    return true;
}

bool ScreenCapture::EnsureStaging(int w, int h) {
    if (stagingTex_ && stagingW_ == w && stagingH_ == h) return true;

    if (stagingTex_) {
        stagingTex_->Release();
        stagingTex_ = nullptr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.BindFlags = 0;

    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &stagingTex_);
    if (FAILED(hr)) return false;

    stagingW_ = w;
    stagingH_ = h;
    return true;
}

bool ScreenCapture::CaptureRegion(int x, int y, int w, int h,
                                  std::vector<uint8_t>& outBuffer,
                                  int timeoutMs, bool& hadError) {
    hadError = false;
    if (!duplication_) { hadError = true; return false; }

    DXGI_OUTDUPL_FRAME_INFO frameInfo;
    IDXGIResource* desktopResource = nullptr;

    HRESULT hr = duplication_->AcquireNextFrame(timeoutMs, &frameInfo, &desktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false; // экран не менялся - это нормально
    }
    if (FAILED(hr)) {
        hadError = true;
        return false;
    }

    ID3D11Texture2D* desktopTex = nullptr;
    hr = desktopResource->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopTex);
    desktopResource->Release();
    if (FAILED(hr)) {
        duplication_->ReleaseFrame();
        hadError = true;
        return false;
    }

    if (!EnsureStaging(w, h)) {
        desktopTex->Release();
        duplication_->ReleaseFrame();
        hadError = true;
        return false;
    }

    D3D11_BOX box;
    box.left = static_cast<UINT>(std::max(0, x));
    box.top = static_cast<UINT>(std::max(0, y));
    box.front = 0;
    box.right = box.left + w;
    box.bottom = box.top + h;
    box.back = 1;

    context_->CopySubresourceRegion(stagingTex_, 0, 0, 0, 0, desktopTex, 0, &box);
    desktopTex->Release();
    duplication_->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped;
    hr = context_->Map(stagingTex_, 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        hadError = true;
        return false;
    }

    outBuffer.resize(static_cast<size_t>(w) * h * 4);
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = outBuffer.data();
    for (int row = 0; row < h; ++row) {
        std::memcpy(dst + row * w * 4, src + row * mapped.RowPitch, static_cast<size_t>(w) * 4);
    }

    context_->Unmap(stagingTex_, 0);
    return true;
}

void ScreenCapture::Shutdown() {
    if (stagingTex_) { stagingTex_->Release(); stagingTex_ = nullptr; }
    if (duplication_) { duplication_->Release(); duplication_ = nullptr; }
    if (context_) { context_->Release(); context_ = nullptr; }
    if (device_) { device_->Release(); device_ = nullptr; }
}
