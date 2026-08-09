#include "ActiveModeHandler.h"

#include <cstdio>
#include <iostream>

bool parsePortCommand(const std::string& arg, sockaddr_in& outAddr) {
    int h1, h2, h3, h4, p1, p2;
    int charsConsumed = 0;
    int fieldsParsed = sscanf_s(arg.c_str(), "%d,%d,%d,%d,%d,%d%n",
        &h1, &h2, &h3, &h4, &p1, &p2, &charsConsumed);

    if (fieldsParsed != 6) return false;
    if (charsConsumed != static_cast<int>(arg.size())) return false;

    auto inRange = [](int v) { return v >= 0 && v <= 255; };
    if (!inRange(h1) || !inRange(h2) || !inRange(h3) || !inRange(h4) ||
        !inRange(p1) || !inRange(p2)) {
        return false;
    }
    if (p1 == 0 && p2 == 0) return false;

    char ipStr[16];
    sprintf_s(ipStr, "%d.%d.%d.%d", h1, h2, h3, h4);

    outAddr.sin_family = AF_INET;
    outAddr.sin_port = htons(static_cast<unsigned short>(p1 * 256 + p2));

    if (inet_pton(AF_INET, ipStr, &outAddr.sin_addr) != 1) return false;

    return true;
}

bool openActiveDataConnection(const sockaddr_in& peerAddr, SOCKET& outDataSock) {
    outDataSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (outDataSock == INVALID_SOCKET) return false;

    if (connect(outDataSock, reinterpret_cast<const sockaddr*>(&peerAddr), sizeof(peerAddr)) == SOCKET_ERROR) {
        closesocket(outDataSock);
        outDataSock = INVALID_SOCKET;
        return false;
    }

    return true;
}
