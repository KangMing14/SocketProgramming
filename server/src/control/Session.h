#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#pragma comment(lib, "ws2_32.lib")

#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <filesystem>
#include "TransferMode.h"

struct ClientSession {
    SOCKET socket;
    bool authenticated = false;
    TransferMode transferMode = TransferMode::Binary;
    std::string username;
    std::filesystem::path currentDir;

    // Populated by PASV/PORT, consumed by STOR/RETR
    bool dataChannelIsPassive = false;
    SOCKET pendingDataSocket = INVALID_SOCKET;
    sockaddr_in pendingPeerAddr{};

    // Populated by RNFR, consumed by RNTO
    std::filesystem::path pendingRenameSource;
    bool hasPendingRename = false;
};

namespace Session {
    void replyWithCode(SOCKET, int, std::string);
    void multilineReplyWithCode(SOCKET, int, std::string);
    SOCKET initializeSession();
    void handleClient(SOCKET);
    void runSession();
}