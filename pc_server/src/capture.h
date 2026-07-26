#pragma once
#include <cstdint>
#include <vector>
#include <d3d11.h>
#include <dxgi1_2.h>

// Захват региона экрана через DXGI Desktop Duplication API.
// Возвращает кадры в формате BGRA8 (плотная упаковка, без паддинга).
class ScreenCapture {
public:
    ~ScreenCapture();

    // outputIndex - индекс монитора (0 = основной), обычно не нужно менять.
    bool Init(int outputIndex = 0);

    // Пытается получить очередной кадр и скопировать регион [x,y,w,h] в outBuffer.
    // timeoutMs - сколько ждать нового кадра от DXGI.
    // Возвращает false, если кадр не готов (таймаут) - это НЕ ошибка, экран просто не менялся.
    // hadError выставляется в true, если произошла настоящая ошибка (устройство потеряно и т.п.)
    bool CaptureRegion(int x, int y, int w, int h,
                       std::vector<uint8_t>& outBuffer,
                       int timeoutMs, bool& hadError);

    void Shutdown();

private:
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    IDXGIOutputDuplication* duplication_ = nullptr;
    ID3D11Texture2D* stagingTex_ = nullptr;
    int stagingW_ = 0;
    int stagingH_ = 0;

    bool EnsureStaging(int w, int h);
};
