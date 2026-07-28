#pragma once

#include <filesystem>
#include <vector>
#include <cstdint>
#include <winsock2.h>
#include "RdtHeader.h"
#include "TransferMode.h"

namespace fs = std::filesystem;

class DataChannelSession {
public:
    DataChannelSession(SOCKET dataSocket, sockaddr_in peerAddr, IRdtTransport& transport);

    bool sendFile(const fs::path& filePath, TransferMode mode = TransferMode::Binary);
    bool receiveFile(const fs::path& destPath, TransferMode mode = TransferMode::Binary);

private:
    // Already-opened UDP socket and the remote peer's address for this
    // particular transfer session, never actually touched directly by 
    // DataChannelSession itself.
    //
    // It's possible the socket and peer address should be owned by the
    // IRdtTransport implementation instead, and DataChannelSession 
    // might not need to store them at all.
    SOCKET dataSocket;
    sockaddr_in peerAddr;

    IRdtTransport& transport;
};