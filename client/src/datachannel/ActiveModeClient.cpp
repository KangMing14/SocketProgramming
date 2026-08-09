#include "ActiveModeClient.h"
#include <ws2tcpip.h>
#include <cstdio>

namespace hybridftp::client {

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

bool parsePortCommand(const std::string& argument, sockaddr_in& outAddress) {
    int h1, h2, h3, h4, p1, p2;
    int charsConsumed = 0;
    const int fieldsParsed = sscanf_s(
        argument.c_str(), "%d,%d,%d,%d,%d,%d%n",
        &h1, &h2, &h3, &h4, &p1, &p2, &charsConsumed);
    if (fieldsParsed != 6 ||
        charsConsumed != static_cast<int>(argument.size())) {
        return false;
    }

    const auto inByteRange = [](int value) {
        return value >= 0 && value <= 255;
    };
    if (!inByteRange(h1) || !inByteRange(h2) || !inByteRange(h3) ||
        !inByteRange(h4) || !inByteRange(p1) || !inByteRange(p2) ||
        (p1 == 0 && p2 == 0)) {
        return false;
    }

    char ipAddress[16];
    sprintf_s(ipAddress, "%d.%d.%d.%d", h1, h2, h3, h4);
    outAddress = {};
    outAddress.sin_family = AF_INET;
    outAddress.sin_port = htons(
        static_cast<unsigned short>(p1 * 256 + p2));
    return inet_pton(AF_INET, ipAddress, &outAddress.sin_addr) == 1;
}

bool openActiveListenPort(const sockaddr_in& localAddress,
                          SOCKET& outDataSock) {
    outDataSock = INVALID_SOCKET;
    if (localAddress.sin_family != AF_INET || localAddress.sin_port == 0) {
        return false;
    }

    outDataSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (outDataSock == INVALID_SOCKET) return false;
    if (bind(outDataSock,
             reinterpret_cast<const sockaddr*>(&localAddress),
             sizeof(localAddress)) == SOCKET_ERROR) {
        closesocket(outDataSock);
        outDataSock = INVALID_SOCKET;
        return false;
    }
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

}
