#pragma once
#include <string>

// Настройки приложения: регион захвата, частота кадров, качество JPEG, порт.
struct Settings {
    int region_x = 0;
    int region_y = 0;
    int region_w = 800;
    int region_h = 1280;
    int fps = 15;
    int jpeg_quality = 75; // 1-100
    int port = 27183;

    // Загружает настройки из простого текстового файла key=value.
    // Если файл не найден — создаёт его со значениями по умолчанию.
    static Settings LoadOrCreate(const std::string& path);
};
