#pragma once

#include <filesystem>
#include <vector>
#include <cstdint>
#include <winsock2.h>

// Member B implements this interface (in rdt/RdtSender.cpp / RdtReceiver.cpp).
// Implement the file-side callers to build and unit-test independently until merge.
class IRdtTransport {
public:
    virtual ~IRdtTransport() = default;
    virtual bool sendChunk(uint32_t seqNum, const char* data, size_t len) = 0;
    virtual bool receiveNext(uint32_t& outSeqNum, std::vector<char>& outData, bool& outIsFinal) = 0;
};

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