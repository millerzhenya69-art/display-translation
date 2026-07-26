#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <vector>

// Простой TCP-сервер: принимает одно подключение (планшет) и шлёт кадры
// в формате [4 байта LE длина][JPEG bytes]. Слушает только на localhost,
// подключение планшета идёт через "adb reverse" по USB.
class TcpFrameServer {
public:
    ~TcpFrameServer();

    bool Start(int port);

    // Блокирует поток до подключения клиента. Возвращает false при остановке сервера.
    bool WaitForClient();

    // Отправляет один кадр. Возвращает false, если клиент отключился (нужно снова WaitForClient).
    bool SendFrame(const std::vector<uint8_t>& jpeg);

    bool HasClient() const { return clientSock_ != INVALID_SOCKET; }

    void CloseClient();
    void Stop();

private:
    SOCKET listenSock_ = INVALID_SOCKET;
    SOCKET clientSock_ = INVALID_SOCKET;
    bool wsaStarted_ = false;
};
