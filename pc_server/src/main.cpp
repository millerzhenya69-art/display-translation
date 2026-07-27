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
static void ComputeAutoRegion(int clientW, int clientH, int desktopW, int desktopH,
                              int& outX, int& outY, int& outW, int& outH) {
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

    std::cout << "=== Display Translation - PC Server ===\n\n";

    ListAvailableOutputs();

    Settings settings = Settings::LoadOrCreate("config.txt");
    std::cout << "Регион захвата по умолчанию: " << settings.region_w << "x" << settings.region_h
              << " (будет автоматически подстроен под экран планшета при подключении)\n";
    std::cout << "FPS: " << settings.fps << ", JPEG quality: " << settings.jpeg_quality
              << ", порт: " << settings.port << ", monitor_index: " << settings.monitor_index << "\n";
    std::cout.flush();

    if (!InitJpegEncoder()) {
        std::cerr << "Не удалось инициализировать JPEG-кодировщик (WIC)\n";
        return 1;
    }

    ScreenCapture capture;
    if (!capture.Init(settings.monitor_index)) {
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
            int monW = 0, monH = 0;
            GetOutputResolution(settings.monitor_index, monW, monH);
            ComputeAutoRegion(static_cast<int>(clientW), static_cast<int>(clientH), monW, monH,
                              regionX, regionY, regionW, regionH);
            std::cout << "Экран планшета: " << clientW << "x" << clientH
                      << " -> регион захвата: " << regionW << "x" << regionH << "\n";
        } else {
            std::cout << "Не получил разрешение экрана от клиента, использую регион из config.txt\n";
        }

        std::cout << "Начинаю трансляцию.\n";

        // Статистика по этапам для диагностики узких мест по производительности
        long long sumCaptureMs = 0, sumEncodeMs = 0, sumSendMs = 0;
        int statFrames = 0;
        auto lastStatsPrint = std::chrono::steady_clock::now();

        while (server.HasClient()) {
            auto frameStart = std::chrono::steady_clock::now();

            auto tCaptureStart = std::chrono::steady_clock::now();

            bool hadError = false;
            bool gotFrame = capture.CaptureRegion(
                regionX, regionY, regionW, regionH,
                bgraBuffer, frameIntervalMs, hadError);

            auto tCaptureEnd = std::chrono::steady_clock::now();

            if (hadError) {
                std::cerr << "Ошибка захвата экрана, переинициализация...\n";
                capture.Shutdown();
                if (!capture.Init(settings.monitor_index)) {
                    std::cerr << "Не удалось восстановить захват экрана\n";
                    server.Stop();
                    ShutdownJpegEncoder();
                    return 1;
                }
                continue;
            }

            if (gotFrame) {
                auto tEncodeStart = std::chrono::steady_clock::now();
                bool encoded = EncodeBgraToJpeg(bgraBuffer.data(), regionW, regionH,
                                     settings.jpeg_quality, jpegBuffer);
                auto tEncodeEnd = std::chrono::steady_clock::now();

                if (encoded) {
                    bool sent = server.SendFrame(jpegBuffer);
                    auto tSendEnd = std::chrono::steady_clock::now();

                    if (!sent) {
                        std::cout << "Планшет отключился.\n";
                        break;
                    }

                    sumCaptureMs += std::chrono::duration_cast<std::chrono::milliseconds>(tCaptureEnd - tCaptureStart).count();
                    sumEncodeMs += std::chrono::duration_cast<std::chrono::milliseconds>(tEncodeEnd - tEncodeStart).count();
                    sumSendMs += std::chrono::duration_cast<std::chrono::milliseconds>(tSendEnd - tEncodeEnd).count();
                    statFrames++;

                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - lastStatsPrint).count() >= 3 && statFrames > 0) {
                        std::cout << "[диагностика] за " << statFrames << " кадров: захват=" << (sumCaptureMs / statFrames)
                                  << "мс, кодирование=" << (sumEncodeMs / statFrames)
                                  << "мс, отправка=" << (sumSendMs / statFrames)
                                  << "мс, факт. FPS=" << (1000.0 / std::max(1LL, (sumCaptureMs + sumEncodeMs + sumSendMs) / statFrames)) << "\n";
                        sumCaptureMs = sumEncodeMs = sumSendMs = 0;
                        statFrames = 0;
                        lastStatsPrint = now;
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
