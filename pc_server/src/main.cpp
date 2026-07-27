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

// Вычисляет оптимальный регион захвата под разрешение экрана клиента:
// подбирает максимальный прямоугольник с пропорциями клиента, вписанный
// в реальный рабочий стол, начиная от левого верхнего угла.
static void ComputeAutoRegion(int clientW, int clientH,
                              int& outX, int& outY, int& outW, int& outH) {
    int desktopW = GetSystemMetrics(SM_CXSCREEN);
    int desktopH = GetSystemMetrics(SM_CYSCREEN);

    if (clientW <= 0 || clientH <= 0 || desktopW <= 0 || desktopH <= 0) {
        outX = 0; outY = 0; outW = 800; outH = 1280;
        return;
    }

    double aspect = static_cast<double>(clientW) / clientH;

    int candidateW = desktopW;
    int candidateH = static_cast<int>(candidateW / aspect);

    if (candidateH > desktopH) {
        candidateH = desktopH;
        candidateW = static_cast<int>(candidateH * aspect);
    }

    outX = 0;
    outY = 0;
    outW = std::max(2, candidateW);
    outH = std::max(2, candidateH);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::cout << "=== Display Translation - PC Server ===\n";

    Settings settings = Settings::LoadOrCreate("config.txt");
    std::cout << "Регион захвата по умолчанию: " << settings.region_w << "x" << settings.region_h
              << " (будет автоматически подстроен под экран планшета при подключении)\n";
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
        std::cout << "Планшет подключён.\n";

        // Рукопожатие: клиент присылает своё разрешение экрана (2x uint32 LE)
        int regionX = settings.region_x, regionY = settings.region_y;
        int regionW = settings.region_w, regionH = settings.region_h;

        uint8_t handshake[8];
        if (server.ReceiveExact(handshake, 8, 3000)) {
            uint32_t clientW = handshake[0] | (handshake[1] << 8) | (handshake[2] << 16) | (uint32_t(handshake[3]) << 24);
            uint32_t clientH = handshake[4] | (handshake[5] << 8) | (handshake[6] << 16) | (uint32_t(handshake[7]) << 24);
            ComputeAutoRegion(static_cast<int>(clientW), static_cast<int>(clientH),
                              regionX, regionY, regionW, regionH);
            std::cout << "Экран планшета: " << clientW << "x" << clientH
                      << " -> регион захвата: " << regionW << "x" << regionH << "\n";
        } else {
            std::cout << "Не получил разрешение экрана от клиента, использую регион из config.txt\n";
        }

        std::cout << "Начинаю трансляцию.\n";

        while (server.HasClient()) {
            auto frameStart = std::chrono::steady_clock::now();

            bool hadError = false;
            bool gotFrame = capture.CaptureRegion(
                regionX, regionY, regionW, regionH,
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
                if (EncodeBgraToJpeg(bgraBuffer.data(), regionW, regionH,
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
