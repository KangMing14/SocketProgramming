#pragma once

#include <filesystem>
#include <vector>
#include <cstdint>
#include <winsock2.h>
#include "../rdt/RdtHeader.h"

class DataChannelSession {
public:
    DataChannelSession(SOCKET dataSocket, sockaddr_in peerAddr, IRdtTransport& transport);

    bool sendFile(const std::filesystem::path& filePath);
    bool receiveFile(const std::filesystem::path& destPath);

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