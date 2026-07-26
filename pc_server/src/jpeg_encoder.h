#pragma once
#include <cstdint>
#include <vector>

// Кодирует BGRA8-буфер (плотная упаковка) в JPEG через Windows Imaging Component (WIC).
// quality: 1-100.
// Возвращает true при успехе, JPEG-байты кладёт в outJpeg.
bool EncodeBgraToJpeg(const uint8_t* bgraData, int width, int height,
                      int quality, std::vector<uint8_t>& outJpeg);

// Вызвать один раз при старте программы и один раз перед выходом.
bool InitJpegEncoder();
void ShutdownJpegEncoder();
