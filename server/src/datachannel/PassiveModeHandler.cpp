#include "PassiveModeHandler.h"

#include <cstdio>
#include <iostream>

bool openPassiveDataPort(SOCKET& outDataSock, unsigned short& outPort) {
    outDataSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (outDataSock == INVALID_SOCKET) {
        std::cerr << "socket() failed, WSAGetLastError = " << WSAGetLastError() << "\n";
        return false;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0; // port 0 => OS pick free port automatically

    if (bind(outDataSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        std::cerr << "bind() failed, WSAGetLastError = " << WSAGetLastError() << "\n";
        closesocket(outDataSock);
        outDataSock = INVALID_SOCKET;
        return false;
    }

    sockaddr_in actual{};
    int len = sizeof(actual);
    if (getsockname(outDataSock, reinterpret_cast<sockaddr*>(&actual), &len) == SOCKET_ERROR) {
        std::cerr << "getsockname() failed, WSAGetLastError = " << WSAGetLastError() << "\n";
        closesocket(outDataSock);
        outDataSock = INVALID_SOCKET;
        return false;
    }

    outPort = ntohs(actual.sin_port);
    return true;
}

std::string formatPasvReply(uint32_t ipv4Address, unsigned short port) {
    unsigned char h1 = (ipv4Address >> 24) & 0xFF;
    unsigned char h2 = (ipv4Address >> 16) & 0xFF;
    unsigned char h3 = (ipv4Address >> 8) & 0xFF;
    unsigned char h4 = ipv4Address & 0xFF;

    unsigned char p1 = (port >> 8) & 0xFF;
    unsigned char p2 = port & 0xFF;

    char buffer[32];
    sprintf_s(buffer, "%d,%d,%d,%d,%d,%d", h1, h2, h3, h4, p1, p2);
    return std::string(buffer);
}