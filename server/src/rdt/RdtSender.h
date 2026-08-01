#pragma once
#include "CheckSum.h"
#include "RdtHeader.h"
#include <string>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <chrono>
#include <deque>
#include <vector>

class RdtSender : public IRdtTransport
{
private:
  SOCKET udpSocket;
  sockaddr_in destAddr;
  int timeoutMs;

  // Helper to physically send the packet
  void sendRawPacket(const RdtPacket &packet);

  struct InFlightPacket {
      uint32_t seq_num;
      std::vector<char> data;
      std::chrono::steady_clock::time_point sent_time;
      bool acked;
      int retries;
  };
  
  std::deque<InFlightPacket> window;

  // Helper to poll ACKs non-blockingly (or blockingly) and retransmit
  bool pollAcksAndRetransmit(bool blocking);

public:
  // Constructor: creates its own UDP socket and sets the destination
  RdtSender(const std::string &targetIp, uint16_t targetPort,
            int timeoutMs = 500);

  // Constructor for PASV mode: reuses an already-bound socket (no socket/bind called).
  // Use this when the server's PassiveModeHandler already owns the socket.
  RdtSender(SOCKET existingSocket, int timeoutMs = 500);

  // Destructor closes the socket
  ~RdtSender();

  // High-level function: Sends a payload and uses Stop-and-Wait reliability
  // Returns true if successfully ACKed, false if failed after max retries
  bool sendChunk(uint32_t seqNum, const char* data, size_t len) override;
  bool receiveNext(uint32_t& outSeqNum, std::vector<char>& outData, bool& outIsFinal) override;
  bool waitForClientReady() override;
  bool flush() override;
};
