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

bool TcpFrameServer::SendRaw(const uint8_t* data, int len) {
    if (clientSock_ == INVALID_SOCKET) return false;
    int sentTotal = 0;
    while (sentTotal < len) {
        int sent = send(clientSock_, (const char*)data + sentTotal, len - sentTotal, 0);
        if (sent == SOCKET_ERROR) {
            CloseClient();
            return false;
        }
        sentTotal += sent;
    }
    return true;
}

bool TcpFrameServer::SendFrame(const std::vector<uint8_t>& data) {
    if (clientSock_ == INVALID_SOCKET) return false;

    uint32_t len = static_cast<uint32_t>(data.size());
    uint8_t header[4] = {
        static_cast<uint8_t>(len & 0xFF),
        static_cast<uint8_t>((len >> 8) & 0xFF),
        static_cast<uint8_t>((len >> 16) & 0xFF),
        static_cast<uint8_t>((len >> 24) & 0xFF),
    };

    if (!SendRaw(header, 4)) return false;
    if (!data.empty() && !SendRaw(data.data(), static_cast<int>(data.size()))) return false;

    return true;
}

bool TcpFrameServer::ReceiveExact(uint8_t* buffer, int len, int timeoutMs) {
    if (clientSock_ == INVALID_SOCKET) return false;

    timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(clientSock_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

    int received = 0;
    while (received < len) {
        int n = recv(clientSock_, (char*)buffer + received, len - received, 0);
        if (n <= 0) {
            return false;
        }
        received += n;
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
