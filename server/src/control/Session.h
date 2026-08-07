#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif

#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <string>
#include <thread>
#include <filesystem>
#include <atomic>
#include <mutex>
#include "TransferMode.h"

enum class DataChannelMode { None, Passive, Active };

struct ClientSession {
    SOCKET socket;
    bool authenticated = false;
    TransferType transferType = TransferType::Binary;
    TransferMode transferMode = TransferMode::Stream;
    std::string username;
    std::filesystem::path currentDir;

    // Populated by PASV/PORT, consumed by STOR/RETR
    DataChannelMode dataChannelMode = DataChannelMode::None;
    SOCKET pendingDataSocket = INVALID_SOCKET;
    sockaddr_in pendingPeerAddr{};
    sockaddr_in controlPeerAddr{};

    // Populated by RNFR, consumed by RNTO
    std::filesystem::path pendingRenameSource;
    bool hasPendingRename = false;

    // The worker owns a consumed data socket while the control thread remains
    // responsive to ABOR.
    std::atomic_bool transferActive{false};
    std::atomic_bool abortRequested{false};
    std::thread transferWorker;
    std::mutex replyMutex;
    std::string pendingUniqueFilename;
};

namespace Session {
    void replyWithCode(SOCKET, int, std::string);
    void multilineReplyWithCode(SOCKET, int, std::string);
    SOCKET initializeSession();
    void handleClient(SOCKET);
    void runSession();
}
