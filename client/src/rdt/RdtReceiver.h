#pragma once
#include "RdtHeader.h"
#include "CheckSum.h"
#include <winsock2.h>
#include <string>
#include <map>

namespace hybridftp::client {

class RdtReceiver : public IRdtTransport
{
private:
    SOCKET udpSocket;
    sockaddr_in localAddr;
    sockaddr_in peerAddr{};
    bool peerKnown = false;
    bool expectedPeerIpSet = false;
    in_addr expectedPeerIp{};
    std::map<uint32_t, std::pair<std::vector<char>, bool>> outOfOrderBuffer;
    uint32_t expectedSequence = 0;

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
    bool signalClientReady(const sockaddr_in& serverDataAddress);
    void expectPeerIp(const in_addr& address);
    bool isValid() const noexcept { return udpSocket != INVALID_SOCKET; }
};

}
