#include "RdtSender.h"
#include <cstring>
#include <iostream>
#include <utility>

#ifndef RDT_DEBUG
#define RDT_DEBUG 0
#endif

namespace hybridftp::client
{

#define MAX_RETRIES 10

  namespace {
  bool sameEndpoint(const sockaddr_in& left, const sockaddr_in& right) {
    return left.sin_family == right.sin_family &&
           left.sin_port == right.sin_port &&
           left.sin_addr.s_addr == right.sin_addr.s_addr;
  }
  }

  bool RdtSender::isAbortRequested() const
  {
    return abortPredicate && abortPredicate();
  }

  void RdtSender::setAbortPredicate(std::function<bool()> predicate)
  {
    abortPredicate = std::move(predicate);
  }

  // Constructor: creates the UDP socket, sets timeout, and saves the destination
  // address
  RdtSender::RdtSender(const std::string &targetIp, uint16_t targetPort,
                       int timeoutMs)
      : timeoutMs(timeoutMs)
  {
    // Step 1: Create a UDP (SOCK_DGRAM) socket
    udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (udpSocket == INVALID_SOCKET)
    {
      std::cerr << "[Sender] Failed to create socket: " << WSAGetLastError()
                << std::endl;
      return;
    }

    // Step 2: Set the receive timeout
    // If recvfrom() waits longer than this, it gives up and returns an error
    // (WSAETIMEDOUT)
    const DWORD timeout = static_cast<DWORD>(timeoutMs);
    if (setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout)) ==
        SOCKET_ERROR) {
      closesocket(udpSocket);
      udpSocket = INVALID_SOCKET;
      return;
    }

    // Step 3: Save the destination address (the receiver's IP and port)
    destAddr.sin_family = AF_INET;
    destAddr.sin_port = htons(targetPort);
    if (targetPort == 0 ||
        inet_pton(AF_INET, targetIp.c_str(), &destAddr.sin_addr) != 1) {
      closesocket(udpSocket);
      udpSocket = INVALID_SOCKET;
    }
  }

  // Constructor for PASV mode: reuses an already-bound socket owned by the
  // server's PassiveModeHandler. Does NOT call socket() or connect().
  RdtSender::RdtSender(SOCKET existingSocket, const sockaddr_in &targetAddr,
                       int timeoutMs)
      : udpSocket(existingSocket), destAddr(targetAddr), timeoutMs(timeoutMs)
  {
    if (!isValid()) return;
    // Just apply the timeout — everything else is already set up by the caller.
    const DWORD timeout = static_cast<DWORD>(timeoutMs);
    if (setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout)) ==
        SOCKET_ERROR) {
      closesocket(udpSocket);
      udpSocket = INVALID_SOCKET;
      return;
    }
#if RDT_DEBUG
    std::cout << "[Sender] Initialized with existing PASV socket." << std::endl;
#endif
  }

  RdtSender::RdtSender(SOCKET existingSocket, int timeoutMs)
      : udpSocket(existingSocket), destAddr{}, timeoutMs(timeoutMs)
  {
    if (!isValid()) return;
    const DWORD timeout = static_cast<DWORD>(timeoutMs);
    if (setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO,
                   reinterpret_cast<const char*>(&timeout), sizeof(timeout)) ==
        SOCKET_ERROR) {
      closesocket(udpSocket);
      udpSocket = INVALID_SOCKET;
    }
  }

  // Destructor: always close the socket to free up OS resources
  RdtSender::~RdtSender()
  {
    if (udpSocket != INVALID_SOCKET)
    {
      closesocket(udpSocket);
    }
  }

  // PRIVATE HELPER: Physically serialize and shoot one packet into the network
  bool RdtSender::sendRawPacket(const RdtPacket &packet)
  {
    // We need a flat byte buffer: [16-byte header][payload bytes]
    char sendBuf[HEADER_SIZE + MAX_PAYLOAD];
    memset(sendBuf, 0, sizeof(sendBuf));

    // Serialize the header (with htonl/htons byte order conversion) into the
    // front
    serializeHeader(packet.header, sendBuf);

    // Copy the payload right after the header
    const uint16_t payload_len = packet.header.payload_len;
    if (payload_len > MAX_PAYLOAD) return false;
    if (payload_len > 0) {
      memcpy(sendBuf + HEADER_SIZE, packet.payload, payload_len);
    }

    // sendto() shoots this flat buffer as a UDP postcard to destAddr
    int totalSize = HEADER_SIZE + payload_len;
    int sent = sendto(udpSocket, sendBuf, totalSize, 0, (sockaddr *)&destAddr,
                      sizeof(destAddr));

    if (sent == SOCKET_ERROR)
    {
      std::cerr << "[Sender] sendto() failed: " << WSAGetLastError() << std::endl;
      return false;
    }
    return sent == totalSize;
  }

  // PRIVATE HELPER: Waits for an ACK packet matching the expected sequence number
  // Returns true if a matching ACK arrived, false if timeout or wrong ACK
  bool RdtSender::waitForAck(uint32_t expected_ack_num)
  {
    while (true)
    {
      if (isAbortRequested())
        return false;
      char recvBuf[HEADER_SIZE + MAX_PAYLOAD];
      sockaddr_in fromAddr{};
      int fromLen = sizeof(fromAddr);

      // recvfrom() blocks here until a packet arrives OR the SO_RCVTIMEO timeout
      // fires
      int n = recvfrom(udpSocket, recvBuf, sizeof(recvBuf), 0,
                       (sockaddr *)&fromAddr, &fromLen);

      if (n == SOCKET_ERROR)
      {
        if (isAbortRequested())
          return false;
#if RDT_DEBUG
        const int err = WSAGetLastError();
        if (err == WSAETIMEDOUT)
        {
          std::cerr << "[Sender] Timeout waiting for ACK " << expected_ack_num
                    << " — will retransmit." << std::endl;
        }
#endif
        return false; // Real timeout, return false so sendChunk will retransmit
      }

      RdtHeader validatedAck{};
      if (!sameEndpoint(fromAddr, destAddr) ||
          !decodeValidatedDatagram(
              recvBuf, static_cast<std::size_t>(n), validatedAck) ||
          !(validatedAck.flags & FLAG_ACK) ||
          (validatedAck.flags & FLAG_DATA) ||
          validatedAck.payload_len != 0) {
        continue;
      }

      // Check if the ACK matches what we were waiting for
      if (validatedAck.ack_num == expected_ack_num)
      {
        return true; // Success!
      }

      // We got an ACK, but for the wrong packet (stale ACK from a previous
      // retransmit)
#if RDT_DEBUG
      std::cerr << "[Sender] Stale ACK received (got " << validatedAck.ack_num
                << " expected " << expected_ack_num << "), ignoring." << std::endl;
#endif
    }
  }

  // PUBLIC: High-level Stop-and-Wait send
  // Builds the packet, sends it, and retransmits on timeout until ACKed
  bool RdtSender::sendChunk(uint32_t seqNum, const char *data, size_t len,
                            bool isFinal)
  {
    if (isAbortRequested())
      return false;
    if (len > MAX_PAYLOAD || (len > 0 && data == nullptr))
      return false;
    // --- Build the packet ---
    RdtPacket packet;
    memset(&packet, 0, sizeof(packet));

    packet.header.seq_num = seqNum;
    packet.header.ack_num = 0;
    packet.header.flags = FLAG_DATA | (isFinal ? FLAG_FIN : 0);
    packet.header.window_size = 1; // Stop-and-Wait: window of 1
    packet.header.payload_len = static_cast<uint16_t>(len);
    packet.header.reserved = 0;
    packet.header.checksum = 0; // Zero out before calculating

    // Copy the file data into the payload slot
    if (len > 0)
      memcpy(packet.payload, data, len);

    // --- Calculate Checksum over the entire packet (header + payload) ---
    // First serialize the header with checksum=0 into a temp buffer
    char tempBuf[HEADER_SIZE + MAX_PAYLOAD];
    memset(tempBuf, 0, sizeof(tempBuf));
    serializeHeader(packet.header, tempBuf);
    if (len > 0)
      memcpy(tempBuf + HEADER_SIZE, packet.payload, len);

    // Compute checksum over [header bytes + payload bytes]
    packet.header.checksum =
        internetChecksum((const uint8_t *)tempBuf, HEADER_SIZE + len);

    // --- Stop-and-Wait Retry Loop ---
    for (int attempt = 0; attempt < MAX_RETRIES; attempt++)
    {
      if (isAbortRequested())
        return false;
#if RDT_DEBUG
      std::cout << "[Sender] Sending seq=" << seqNum << " (attempt "
                << attempt + 1 << "/" << MAX_RETRIES << ")" << std::endl;
#endif

      if (!sendRawPacket(packet)) continue;

      if (waitForAck(seqNum))
      {
#if RDT_DEBUG
        std::cout << "[Sender] ACK received for seq=" << seqNum << std::endl;
#endif
        return true; // Successfully delivered!
      }
      // If we got here, the ACK didn't come in time — loop and retransmit
    }

    std::cerr << "[Sender] FAILED to deliver seq=" << seqNum << " after "
              << MAX_RETRIES << " attempts." << std::endl;
    return false;
  }

  bool RdtSender::receiveNext(uint32_t &, std::vector<char> &, bool &)
  {
    // RdtSender does not receive data chunks.
    return false;
  }

  bool RdtSender::waitForServerReady(const in_addr &expectedServerIp)
  {
    if (!isValid() || isAbortRequested())
      return false;

    DWORD timeout = HANDSHAKE_TIMEOUT_MS;
    setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char *>(&timeout), sizeof(timeout));

    for (int attempt = 0; attempt < HANDSHAKE_MAX_RETRIES; ++attempt)
    {
      if (isAbortRequested())
        return false;
      char receiveBuffer[HEADER_SIZE + MAX_PAYLOAD];
      sockaddr_in from{};
      int fromLength = sizeof(from);
      const int received = recvfrom(
          udpSocket, receiveBuffer, sizeof(receiveBuffer), 0,
          reinterpret_cast<sockaddr *>(&from), &fromLength);
      if (received == SOCKET_ERROR)
      {
        if (isAbortRequested())
          return false;
        if (WSAGetLastError() == WSAETIMEDOUT)
          continue;
        return false;
      }
      RdtHeader syn{};
      if (from.sin_addr.s_addr != expectedServerIp.s_addr ||
          !decodeValidatedDatagram(
              receiveBuffer, static_cast<std::size_t>(received), syn) ||
          !(syn.flags & FLAG_SYN) || (syn.flags & FLAG_DATA) ||
          syn.payload_len != 0)
        continue;
      destAddr = from;

      RdtHeader ack{};
      ack.ack_num = syn.seq_num;
      ack.flags = FLAG_ACK;
      ack.window_size = static_cast<std::uint16_t>(RDT_RECEIVE_WINDOW);
      char ackBuffer[HEADER_SIZE]{};
      serializeHeader(ack, ackBuffer);
      ack.checksum = internetChecksum(
          reinterpret_cast<const uint8_t *>(ackBuffer), HEADER_SIZE);
      serializeHeader(ack, ackBuffer);

      DWORD dataTimeout = static_cast<DWORD>(timeoutMs);
      setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char *>(&dataTimeout), sizeof(dataTimeout));
      return sendto(udpSocket, ackBuffer, static_cast<int>(HEADER_SIZE), 0,
                    reinterpret_cast<const sockaddr *>(&destAddr),
                    sizeof(destAddr)) != SOCKET_ERROR;
    }
    return false;
  }

}
