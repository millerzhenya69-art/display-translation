#include "tcp_server.h"
#include <cstring>

TcpFrameServer::~TcpFrameServer() {
    Stop();
}

bool TcpFrameServer::Start(int port) {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return false;
    wsaStarted_ = true;

    listenSock_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSock_ == INVALID_SOCKET) return false;

    int opt = 1;
    setsockopt(listenSock_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    // слушаем только на localhost - планшет заходит через adb reverse
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(listenSock_, (sockaddr*)&addr, sizeof(addr)) == SOCKET_ERROR) return false;
    if (listen(listenSock_, 1) == SOCKET_ERROR) return false;

    return true;
}

bool TcpFrameServer::WaitForClient() {
    if (listenSock_ == INVALID_SOCKET) return false;
    sockaddr_in clientAddr = {};
    int addrLen = sizeof(clientAddr);
    clientSock_ = accept(listenSock_, (sockaddr*)&clientAddr, &addrLen);
    return clientSock_ != INVALID_SOCKET;
}

bool TcpFrameServer::SendFrame(const std::vector<uint8_t>& jpeg) {
    if (clientSock_ == INVALID_SOCKET) return false;

    uint32_t len = static_cast<uint32_t>(jpeg.size());
    uint8_t header[4] = {
        static_cast<uint8_t>(len & 0xFF),
        static_cast<uint8_t>((len >> 8) & 0xFF),
        static_cast<uint8_t>((len >> 16) & 0xFF),
        static_cast<uint8_t>((len >> 24) & 0xFF),
    };

    if (send(clientSock_, (const char*)header, 4, 0) != 4) {
        CloseClient();
        return false;
    }

    size_t sentTotal = 0;
    const char* data = reinterpret_cast<const char*>(jpeg.data());
    while (sentTotal < jpeg.size()) {
        int sent = send(clientSock_, data + sentTotal,
                        static_cast<int>(jpeg.size() - sentTotal), 0);
        if (sent == SOCKET_ERROR) {
            CloseClient();
            return false;
        }
        sentTotal += sent;
    }

    return true;
}

void TcpFrameServer::CloseClient() {
    if (clientSock_ != INVALID_SOCKET) {
        closesocket(clientSock_);
        clientSock_ = INVALID_SOCKET;
    }
}

void TcpFrameServer::Stop() {
    CloseClient();
    if (listenSock_ != INVALID_SOCKET) {
        closesocket(listenSock_);
        listenSock_ = INVALID_SOCKET;
    }
    if (wsaStarted_) {
        WSACleanup();
        wsaStarted_ = false;
    }
}
