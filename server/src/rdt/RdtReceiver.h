#pragma once
#include "RdtHeader.h"
#include "CheckSum.h"
#include <winsock2.h>
#include <string>

class RdtReceiver : public IRdtTransport
{
private:
    SOCKET udpSocket;
    sockaddr_in localAddr;

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
    bool sendChunk(uint32_t seqNum, const char* data, size_t len) override;
};
