#pragma once
#include "CheckSum.h"
#include "RdtHeader.h"
#include <string>
#include <winsock2.h>

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
  // Constructor initializes the socket and sets the destination IP and port
  RdtSender(const std::string &targetIp, uint16_t targetPort,
            int timeoutMs = 500);

  // Destructor closes the socket
  ~RdtSender();

  // High-level function: Sends a payload and uses Stop-and-Wait reliability
  // Returns true if successfully ACKed, false if failed after max retries
  bool sendChunk(uint32_t seqNum, const char* data, size_t len) override;
  bool receiveNext(uint32_t& outSeqNum, std::vector<char>& outData, bool& outIsFinal) override;
};
