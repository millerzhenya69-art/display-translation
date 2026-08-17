// Display Translation - PC Server
// Захватывает заданный регион экрана, кодирует в H.264 и стримит на планшет
// через TCP (планшет подключается через "adb reverse" по USB).

#include "tcp_server.h"   // winsock2.h должен быть включён раньше windows.h
#include "capture.h"
#include "h264_encoder.h"
#include "config.h"

#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
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

// Точка передачи кадров из потока захвата в поток кодирования/отправки.
// Модель "последний кадр побеждает": поток захвата НИКОГДА не блокируется и не ждёт
// кодирование/отправку - если та сторона тормозит, старые кадры просто перезаписываются
// более новыми вместо накопления очереди и нарастающей задержки. Это раньше было
// главным узким местом: захват, конвертация в NV12, кодирование и отправка шли
// строго последовательно в одном потоке, и любой всплеск времени на одном из этапов
// (например, программный H.264-энкодер) сразу срывал темп всего конвейера.
struct FrameHandoff {
    std::mutex mtx;
    std::condition_variable cv;
    std::vector<uint8_t> frame;
    uint64_t version = 0;
    bool hasFrame = false;
};

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    std::cout << "=== Display Translation - PC Server (H.264) ===\n\n";

    ListAvailableOutputs();

    Settings settings = Settings::LoadOrCreate("config.txt");
    std::cout << "Регион захвата по умолчанию: " << settings.region_w << "x" << settings.region_h
              << " (будет автоматически подстроен под экран планшета при подключении)\n";
    std::cout << "FPS: " << settings.fps << ", битрейт: " << settings.bitrate_kbps
              << " кбит/с, порт: " << settings.port << ", monitor_index: " << settings.monitor_index << "\n";
    std::cout.flush();

    ScreenCapture capture;
    if (!capture.Init(settings.monitor_index)) {
        std::cerr << "Не удалось инициализировать захват экрана (DXGI Desktop Duplication)\n";
        return 1;
    }

    TcpFrameServer server;
    if (!server.Start(settings.port)) {
        std::cerr << "Не удалось запустить TCP-сервер на порту " << settings.port << "\n";
        capture.Shutdown();
        return 1;
    }

    std::cout << "\nСервер запущен. На ПК выполните (в отдельном терминале, планшет подключен по USB):\n";
    std::cout << "  adb reverse tcp:" << settings.port << " tcp:" << settings.port << "\n";
    std::cout << "Затем запустите приложение на планшете.\n\n";

    const int frameIntervalMs = 1000 / std::max(1, settings.fps);

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

        H264Encoder encoder;
        // NV12 требует чётных размеров - округляем ЗАРАНЕЕ, чтобы сообщить клиенту точные итоговые размеры.
        regionW &= ~1;
        regionH &= ~1;

        // Сообщаем клиенту итоговый размер кадра (нужен для настройки MediaCodec на Android)
        uint8_t sizeResp[8];
        uint32_t rw = static_cast<uint32_t>(regionW), rh = static_cast<uint32_t>(regionH);
        sizeResp[0] = rw & 0xFF; sizeResp[1] = (rw >> 8) & 0xFF; sizeResp[2] = (rw >> 16) & 0xFF; sizeResp[3] = (rw >> 24) & 0xFF;
        sizeResp[4] = rh & 0xFF; sizeResp[5] = (rh >> 8) & 0xFF; sizeResp[6] = (rh >> 16) & 0xFF; sizeResp[7] = (rh >> 24) & 0xFF;
        if (!server.SendRaw(sizeResp, 8)) {
            std::cerr << "Не удалось отправить размер кадра клиенту\n";
            server.CloseClient();
            continue;
        }

        if (!encoder.Init(regionW, regionH, settings.fps, settings.bitrate_kbps * 1000)) {
            std::cerr << "Не удалось инициализировать H.264-кодировщик\n";
            server.CloseClient();
            continue;
        }

        std::cout << "Начинаю трансляцию.\n";

        FrameHandoff handoff;
        std::atomic<bool> streaming{true};
        std::atomic<bool> fatalCaptureError{false};

        // Поток захвата: крутится независимо от кодирования/отправки. Как только
        // DXGI отдаёт новый кадр, он сразу публикуется для потока кодирования - тому
        // больше не нужно ждать AcquireNextFrame внутри общего конвейера.
        std::thread captureThread([&]() {
            std::vector<uint8_t> localBuf;
            while (streaming.load(std::memory_order_relaxed)) {
                bool hadError = false;
                bool got = capture.CaptureRegion(regionX, regionY, regionW, regionH,
                                                  localBuf, frameIntervalMs, hadError);
                if (hadError) {
                    std::cerr << "Ошибка захвата экрана, переинициализация...\n";
                    capture.Shutdown();
                    if (!capture.Init(settings.monitor_index)) {
                        std::cerr << "Не удалось восстановить захват экрана\n";
                        fatalCaptureError.store(true);
                        streaming.store(false);
                        handoff.cv.notify_all();
                        break;
                    }
                    continue;
                }
                if (got) {
                    std::lock_guard<std::mutex> lock(handoff.mtx);
                    handoff.frame.swap(localBuf); // O(1), без копирования пикселей
                    handoff.version++;
                    handoff.hasFrame = true;
                    handoff.cv.notify_one();
                }
            }
        });

        // Кодирование + отправка: работает в собственном темпе (frameIntervalMs) и всегда
        // берёт САМЫЙ СВЕЖИЙ кадр захвата. Как и раньше, кодируем повторяющиеся кадры,
        // если экран не менялся - H.264-энкодеру нужен непрерывный поток кадров.
        std::vector<uint8_t> lastGoodFrame;
        std::vector<uint8_t> nalBuffer;
        uint64_t lastSeenVersion = 0;

        long long sumWaitMs = 0, sumEncodeMs = 0, sumSendMs = 0;
        int statFrames = 0;
        auto lastStatsPrint = std::chrono::steady_clock::now();

        while (server.HasClient() && streaming.load(std::memory_order_relaxed)) {
            auto frameStart = std::chrono::steady_clock::now();

            {
                std::unique_lock<std::mutex> lock(handoff.mtx);
                handoff.cv.wait_for(lock, std::chrono::milliseconds(frameIntervalMs), [&]{
                    return handoff.version != lastSeenVersion || !streaming.load(std::memory_order_relaxed);
                });
                if (handoff.hasFrame && handoff.version != lastSeenVersion) {
                    lastGoodFrame.swap(handoff.frame); // O(1) обмен буферами, без memcpy
                    lastSeenVersion = handoff.version;
                }
            }
            auto tWaitEnd = std::chrono::steady_clock::now();

            if (!streaming.load(std::memory_order_relaxed)) break;

            if (!lastGoodFrame.empty()) {
                auto tEncodeStart = tWaitEnd;
                bool encoded = encoder.EncodeFrame(lastGoodFrame.data(), nalBuffer);
                auto tEncodeEnd = std::chrono::steady_clock::now();

                if (encoded) {
                    bool sent = server.SendFrame(nalBuffer);
                    auto tSendEnd = std::chrono::steady_clock::now();

                    if (!sent) {
                        std::cout << "Планшет отключился.\n";
                        break;
                    }

                    sumWaitMs += std::chrono::duration_cast<std::chrono::milliseconds>(tWaitEnd - frameStart).count();
                    sumEncodeMs += std::chrono::duration_cast<std::chrono::milliseconds>(tEncodeEnd - tEncodeStart).count();
                    sumSendMs += std::chrono::duration_cast<std::chrono::milliseconds>(tSendEnd - tEncodeEnd).count();
                    statFrames++;

                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - lastStatsPrint).count() >= 3 && statFrames > 0) {
                        long long totalMs = sumWaitMs + sumEncodeMs + sumSendMs;
                        std::cout << "[диагностика] за " << statFrames << " кадров: ожидание_кадра=" << (sumWaitMs / statFrames)
                                  << "мс, кодирование=" << (sumEncodeMs / statFrames)
                                  << "мс, отправка=" << (sumSendMs / statFrames)
                                  << "мс, факт. FPS=" << (1000.0 * statFrames / std::max(1LL, totalMs)) << "\n";
                        sumWaitMs = sumEncodeMs = sumSendMs = 0;
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

        streaming.store(false);
        handoff.cv.notify_all();
        captureThread.join();

        encoder.Shutdown();

        if (fatalCaptureError.load()) {
            server.Stop();
            return 1;
        }
    }

    capture.Shutdown();
    return 0;
}
