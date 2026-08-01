#include "ActiveModeClient.h"
#include <ws2tcpip.h>
#include <cstdio>

bool getLocalIPv4ForServer(const std::string& serverIp, uint32_t& outIpv4) {
    SOCKET probeSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (probeSock == INVALID_SOCKET) return false;

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(9);
    if (inet_pton(AF_INET, serverIp.c_str(), &serverAddr.sin_addr) != 1) {
        closesocket(probeSock);
        return false;
    }

    if (connect(probeSock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        closesocket(probeSock);
        return false;
    }

    sockaddr_in localAddr{};
    int len = sizeof(localAddr);
    if (getsockname(probeSock, reinterpret_cast<sockaddr*>(&localAddr), &len) == SOCKET_ERROR) {
        closesocket(probeSock);
        return false;
    }

    outIpv4 = ntohl(localAddr.sin_addr.s_addr);
    closesocket(probeSock);
    return true;
}

bool openActiveListenPort(SOCKET& outDataSock, unsigned short& outPort) {
    outDataSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (outDataSock == INVALID_SOCKET) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0;

    if (bind(outDataSock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(outDataSock);
        outDataSock = INVALID_SOCKET;
        return false;
    }

    sockaddr_in actual{};
    int len = sizeof(actual);
    if (getsockname(outDataSock, reinterpret_cast<sockaddr*>(&actual), &len) == SOCKET_ERROR) {
        closesocket(outDataSock);
        outDataSock = INVALID_SOCKET;
        return false;
    }

    outPort = ntohs(actual.sin_port);
    return true;
}

std::string formatPortCommand(uint32_t ipv4Address, unsigned short port) {
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