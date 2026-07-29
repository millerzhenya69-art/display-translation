#pragma once
#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdint>
#include <vector>

// Простой TCP-сервер: принимает одно подключение (планшет) и шлёт кадры
// в формате [4 байта LE длина][H.264 NAL bytes]. Слушает только на localhost,
// подключение планшета идёт через "adb reverse" по USB.
class TcpFrameServer {
public:
    ~TcpFrameServer();

    bool Start(int port);

    // Блокирует поток до подключения клиента. Возвращает false при остановке сервера.
    bool WaitForClient();

    // Отправляет один кадр (с префиксом длины). Возвращает false, если клиент отключился.
    bool SendFrame(const std::vector<uint8_t>& data);

    // Отправляет len байт без префикса длины (используется для служебных сообщений протокола).
    bool SendRaw(const uint8_t* data, int len);

    // Читает ровно len байт с блокировкой (используется для рукопожатия с разрешением экрана клиента).
    bool ReceiveExact(uint8_t* buffer, int len, int timeoutMs);

    bool HasClient() const { return clientSock_ != INVALID_SOCKET; }

    void CloseClient();
    void Stop();

private:
    SOCKET listenSock_ = INVALID_SOCKET;
    SOCKET clientSock_ = INVALID_SOCKET;
    bool wsaStarted_ = false;
};
