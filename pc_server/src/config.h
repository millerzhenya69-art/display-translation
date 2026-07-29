#pragma once
#include <string>

// Настройки приложения: регион захвата, частота кадров, битрейт видео, порт.
struct Settings {
    int region_x = 0;
    int region_y = 0;
    int region_w = 800;
    int region_h = 1280;
    int fps = 30;
    int jpeg_quality = 75; // не используется с переходом на H.264, оставлено для совместимости
    int bitrate_kbps = 4000; // битрейт H.264-потока в кбит/с
    int port = 27183;
    int monitor_index = 0; // индекс монитора для захвата (0 = основной; смотрите список при запуске сервера)

    // Загружает настройки из простого текстового файла key=value.
    // Если файл не найден — создаёт его со значениями по умолчанию.
    static Settings LoadOrCreate(const std::string& path);
};
