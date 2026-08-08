#pragma once
#include "RdtHeader.h"
#include "CheckSum.h"
#include <winsock2.h>
#include <string>
#include <map>
#include <functional>
#include <vector>

namespace hybridftp::client {

class RdtReceiver : public IRdtTransport
{
private:
    SOCKET udpSocket = INVALID_SOCKET;
    sockaddr_in localAddr{};
    sockaddr_in peerAddr{};
    bool peerKnown = false;
    bool expectedPeerIpSet = false;
    in_addr expectedPeerIp{};
    std::map<uint32_t, std::pair<std::vector<char>, bool>> outOfOrderBuffer;
    uint32_t expectedSequence = 0;
    std::vector<char> pendingDatagram;
    sockaddr_in pendingFrom{};
    std::function<bool()> abortPredicate;
    bool finalAcknowledgementPending = false;
    uint32_t pendingFinalSequence = 0;

    bool sendControlResponse(uint32_t sequence, std::uint8_t flags,
                             const sockaddr_in& clientAddr);
    bool sendAck(uint32_t sequence, const sockaddr_in& clientAddr);
    bool sendNak(uint32_t sequence, const sockaddr_in& clientAddr);
    bool isAbortRequested() const;
    void applyDataTimeout();

public:
    // Constructor binds the socket to a specific port to listen
    RdtReceiver(uint16_t listenPort);

    // Constructor for PASV mode: reuses an already-bound socket (no socket/bind called).
    // Use this when the server's PassiveModeHandler already owns the socket.
    RdtReceiver(SOCKET existingSocket);

    // Destructor closes the socket
    ~RdtReceiver();

    // Waits for a valid packet and verifies its checksum. The final packet is
    // acknowledged only after confirmReceive() reports a successful file commit.
    // Returns true on success, false on error/timeout.
    // Populates outData with the file chunk, outSeqNum with the sequence number, and outIsFinal if it's the last chunk.
    bool receiveNext(uint32_t& outSeqNum, std::vector<char>& outData, bool& outIsFinal) override;
    bool confirmReceive(uint32_t seqNum, bool accepted) override;
    bool sendChunk(uint32_t seqNum, const char* data, size_t len,
                   bool isFinal = false) override;
    bool flush() override { return true; }
    bool signalClientReady(const sockaddr_in& serverDataAddress);
    void expectPeerIp(const in_addr& address);
    bool isValid() const noexcept { return udpSocket != INVALID_SOCKET; }
    void setAbortPredicate(std::function<bool()> predicate);
};

}
