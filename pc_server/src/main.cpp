// Display Translation - PC Server
// Захватывает заданный регион экрана, кодирует в JPEG и стримит на планшет
// через TCP (планшет подключается через "adb reverse" по USB).

#include "tcp_server.h"   // winsock2.h должен быть включён раньше windows.h
#include "capture.h"
#include "jpeg_encoder.h"
#include "config.h"

#include <chrono>
#include <thread>
#include <iostream>
#include <algorithm>

int main() {
    std::cout << "=== Display Translation - PC Server ===\n";

    Settings settings = Settings::LoadOrCreate("config.txt");
    std::cout << "Регион захвата: " << settings.region_w << "x" << settings.region_h
              << " (offset " << settings.region_x << "," << settings.region_y << ")\n";
    std::cout << "FPS: " << settings.fps << ", JPEG quality: " << settings.jpeg_quality
              << ", порт: " << settings.port << "\n";

    if (!InitJpegEncoder()) {
        std::cerr << "Не удалось инициализировать JPEG-кодировщик (WIC)\n";
        return 1;
    }

    ScreenCapture capture;
    if (!capture.Init(0)) {
        std::cerr << "Не удалось инициализировать захват экрана (DXGI Desktop Duplication)\n";
        ShutdownJpegEncoder();
        return 1;
    }

    TcpFrameServer server;
    if (!server.Start(settings.port)) {
        std::cerr << "Не удалось запустить TCP-сервер на порту " << settings.port << "\n";
        capture.Shutdown();
        ShutdownJpegEncoder();
        return 1;
    }

    std::cout << "\nСервер запущен. На ПК выполните (в отдельном терминале, планшет подключен по USB):\n";
    std::cout << "  adb reverse tcp:" << settings.port << " tcp:" << settings.port << "\n";
    std::cout << "Затем запустите приложение на планшете.\n\n";

    const int frameIntervalMs = 1000 / std::max(1, settings.fps);
    std::vector<uint8_t> bgraBuffer;
    std::vector<uint8_t> jpegBuffer;

    while (true) {
        std::cout << "Ожидание подключения планшета...\n";
        if (!server.WaitForClient()) {
            std::cerr << "Ошибка ожидания клиента\n";
            break;
        }
        std::cout << "Планшет подключён. Начинаю трансляцию.\n";

        while (server.HasClient()) {
            auto frameStart = std::chrono::steady_clock::now();

            bool hadError = false;
            bool gotFrame = capture.CaptureRegion(
                settings.region_x, settings.region_y,
                settings.region_w, settings.region_h,
                bgraBuffer, frameIntervalMs, hadError);

            if (hadError) {
                std::cerr << "Ошибка захвата экрана, переинициализация...\n";
                capture.Shutdown();
                if (!capture.Init(0)) {
                    std::cerr << "Не удалось восстановить захват экрана\n";
                    server.Stop();
                    ShutdownJpegEncoder();
                    return 1;
                }
                continue;
            }

            if (gotFrame) {
                if (EncodeBgraToJpeg(bgraBuffer.data(), settings.region_w, settings.region_h,
                                     settings.jpeg_quality, jpegBuffer)) {
                    if (!server.SendFrame(jpegBuffer)) {
                        std::cout << "Планшет отключился.\n";
                        break;
                    }
                }
            }

            auto frameEnd = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                frameEnd - frameStart).count();
            auto sleepMs = frameIntervalMs - elapsed;
            if (sleepMs > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
            }
        }
    }

    capture.Shutdown();
    ShutdownJpegEncoder();
    return 0;
}
