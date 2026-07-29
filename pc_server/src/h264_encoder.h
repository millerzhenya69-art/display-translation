#pragma once
#include <cstdint>
#include <vector>

// H.264-кодировщик на основе Windows Media Foundation (программный, без сторонних библиотек).
// Кормим кадры в формате BGRA8, на выходе получаем Annex-B поток (со стартовыми кодами 00 00 00 01),
// пригодный для скармливания напрямую в Android MediaCodec.
class H264Encoder {
public:
    ~H264Encoder();

    bool Init(int width, int height, int fps, int bitrateBps);

    // Кодирует один кадр. При успехе кладёт в outNalUnits один или несколько NAL-юнитов
    // (Annex-B, конкатенированные), которые нужно отправить клиенту как один "access unit".
    // Возвращает false, если кодировщик пока не выдал данных для этого кадра (нормально для первых кадров).
    bool EncodeFrame(const uint8_t* bgraData, std::vector<uint8_t>& outNalUnits);

    void Shutdown();

private:
    void* transform_ = nullptr;   // IMFTransform*
    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    int64_t frameDuration_ = 0;   // в единицах 100-нс (MF time)
    int64_t frameTime_ = 0;
    std::vector<uint8_t> nv12Buffer_;
    bool started_ = false;

    void ConvertBgraToNv12(const uint8_t* bgra, uint8_t* nv12);
};
