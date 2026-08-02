#pragma once
#include "RdtHeader.h"
#include "CheckSum.h"
#include <winsock2.h>
#include <string>
#include <map>
#include <vector>

class RdtReceiver : public IRdtTransport
{
private:
    SOCKET udpSocket;
    sockaddr_in localAddr;
    std::map<uint32_t, std::pair<std::vector<char>, bool>> outOfOrderBuffer;
    uint32_t expected_seq = 0;
    sockaddr_in peerAddr{};
    bool peerKnown = false;
    std::vector<char> pendingDatagram;
    sockaddr_in pendingFrom{};

    // Helper to send an ACK back to whatever address just sent us data
    void sendAck(uint32_t ack_num, sockaddr_in &clientAddr);

public:
    // Constructor binds the socket to a specific port to listen
    RdtReceiver(uint16_t listenPort);

    // Constructor for PASV mode: reuses an already-bound socket (no socket/bind called).
    // Use this when the server's PassiveModeHandler already owns the socket.
    RdtReceiver(SOCKET existingSocket);

    // Destructor closes the socket
    ~RdtReceiver();

    // High-level function: Waits for a valid packet, verifies checksum, and auto-sends an ACK.
    // Returns true on success, false on error/timeout.
    // Populates outData with the file chunk, outSeqNum with the sequence number, and outIsFinal if it's the last chunk.
    bool receiveNext(uint32_t& outSeqNum, std::vector<char>& outData, bool& outIsFinal) override;
    bool sendChunk(uint32_t seqNum, const char* data, size_t len,
                   bool isFinal = false) override;
    bool flush() override { return true; }
    bool isValid() const noexcept { return udpSocket != INVALID_SOCKET; }
    bool initiateActiveHandshake(const sockaddr_in& expectedPeer);
};
