#pragma once
#include "CheckSum.h"
#include "RdtHeader.h"
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>

namespace hybridftp::client {

class RdtSender : public IRdtTransport
{
private:
  SOCKET udpSocket;
  sockaddr_in destAddr;
  int timeoutMs;

  // Helper to physically send the packet
  void sendRawPacket(const RdtPacket &packet);

  // Helper to wait for the ACK
  bool waitForAck(uint32_t expected_ack_num);

public:
  // Constructor: creates its own UDP socket and sets the destination
  RdtSender(const std::string &targetIp, uint16_t targetPort,
            int timeoutMs = 500);

  // Constructor for PASV mode: reuses an already-bound socket (no socket/bind called).
  // Use this when the server's PassiveModeHandler already owns the socket.
  RdtSender(SOCKET existingSocket, const sockaddr_in &targetAddr,
            int timeoutMs = 500);

  // Active STOR: destination is learned from the server's SYN packet.
  RdtSender(SOCKET existingSocket, int timeoutMs = 500);

  // Destructor closes the socket
  ~RdtSender();

  // High-level function: Sends a payload and uses Stop-and-Wait reliability
  // Returns true if successfully ACKed, false if failed after max retries
  bool sendChunk(uint32_t seqNum, const char* data, size_t len,
                 bool isFinal = false) override;
  bool receiveNext(uint32_t& outSeqNum, std::vector<char>& outData, bool& outIsFinal) override;
  bool flush() override { return true; }
  bool waitForServerReady(const in_addr& expectedServerIp);
  bool isValid() const noexcept { return udpSocket != INVALID_SOCKET; }
};

}
