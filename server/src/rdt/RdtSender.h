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
#include <functional>

class RdtSender : public IRdtTransport
{
  friend class RdtTestAccess;

private:
  static constexpr double RTT_ALPHA = 0.125;
  static constexpr double RTT_BETA = 0.25;
  static constexpr int MIN_TIMEOUT_MS = 50;
  static constexpr int MAX_TIMEOUT_MS = 2000;
  static constexpr double MIN_CWND = 1.0;
  static constexpr double INITIAL_CWND = 4.0;

  SOCKET udpSocket;
  sockaddr_in destAddr;
  int timeoutMs;

  double estimatedRttMs;
  double devRttMs;
  double cwnd;
  size_t cleanAcks;

  // Test seams
  std::function<int(SOCKET, const char*, int, int, const sockaddr*, int)> injectedSendTo;
  std::function<void()> injectedPreSendDelay;

  // Helper to physically send the packet
  bool sendRawPacket(const RdtPacket &packet, std::chrono::steady_clock::time_point &outSendTime);

  struct InFlightPacket {
      uint32_t seq_num;
      std::vector<char> data;
      std::chrono::steady_clock::time_point sent_time;
      bool acked;
      bool retransmitted;
      int retries;
  };
  
  std::deque<InFlightPacket> window;

  size_t effectiveWindowSize() const;
  void updateRtt(double sampleRttMs);
  void applyCongestionDecrease();

  // Helper to poll ACKs non-blockingly (or blockingly) and retransmit
  bool pollAcksAndRetransmit(bool blocking);

public:
  // Testability accessors
  double getCongestionWindow() const noexcept;
  int getTimeoutMs() const noexcept;
  double getEstimatedRttMs() const noexcept;
  double getDevRttMs() const noexcept;
  size_t getCleanAckCount() const noexcept;

  // Constructor: creates its own UDP socket and sets the destination
  RdtSender(const std::string &targetIp, uint16_t targetPort,
            int initialTimeoutMs = 500);

  // Constructor for PASV mode: reuses an already-bound socket (no socket/bind called).
  // Use this when the server's PassiveModeHandler already owns the socket.
  RdtSender(SOCKET existingSocket, int initialTimeoutMs = 500);

  // Destructor closes the socket
  ~RdtSender();

  // High-level function: Sends a payload and uses Stop-and-Wait reliability
  // Returns true if successfully ACKed, false if failed after max retries
  bool sendChunk(uint32_t seqNum, const char* data, size_t len) override;
  bool receiveNext(uint32_t& outSeqNum, std::vector<char>& outData, bool& outIsFinal) override;
  bool waitForClientReady() override;
  bool flush() override;
};
