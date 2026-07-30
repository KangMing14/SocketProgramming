#include "RdtSender.h"
#include <cstring>
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <thread>

#include "../common/ProtocolConstants.h"

#define MAX_RETRIES 10

// ---- CHAOS MODE ----
// Set to 1 to inject artificial network faults for testing reliability.
// Leave as 1 for normal operation.
#define CHAOS_MODE 1

#if CHAOS_MODE
#define CHAOS_DROP_PERCENT 15    // Drop 15% of outgoing packets
#define CHAOS_CORRUPT_PERCENT 5  // Flip a bit in 5% of payloads
#define CHAOS_LATENCY_MIN_MS 200 // Minimum artificial latency in ms
#define CHAOS_LATENCY_MAX_MS 400 // Maximum artificial latency in ms
#endif

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
  DWORD timeout = static_cast<DWORD>(timeoutMs);
  setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
             sizeof(timeout));

  // Step 3: Save the destination address (the receiver's IP and port)
  memset(&destAddr, 0, sizeof(destAddr));
  destAddr.sin_family = AF_INET;
  destAddr.sin_port = htons(targetPort);
  inet_pton(AF_INET, targetIp.c_str(), &destAddr.sin_addr);
}

// Constructor for PASV mode: reuses an already-bound socket owned by the
// server's PassiveModeHandler. Does NOT call socket() or connect().
RdtSender::RdtSender(SOCKET existingSocket, int timeoutMs)
    : timeoutMs(timeoutMs), udpSocket(existingSocket)
{
  memset(&destAddr, 0, sizeof(destAddr));
  // Just apply the timeout — everything else is already set up by the caller.
  DWORD timeout = static_cast<DWORD>(timeoutMs);
  setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
             sizeof(timeout));
  std::cout << "[Sender] Initialized with existing PASV socket." << std::endl;
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
void RdtSender::sendRawPacket(const RdtPacket &packet)
{
  // We need a flat byte buffer: [16-byte header][payload bytes]
  char sendBuf[HEADER_SIZE + MAX_PAYLOAD];
  memset(sendBuf, 0, sizeof(sendBuf));

  // Serialize the header (with htonl/htons byte order conversion) into the front
  serializeHeader(packet.header, sendBuf);

  // Copy the payload right after the header
  uint16_t payload_len = packet.header.payload_len;
  if (payload_len > MAX_PAYLOAD)
    payload_len = MAX_PAYLOAD;
  memcpy(sendBuf + HEADER_SIZE, packet.payload, payload_len);

#if CHAOS_MODE
  // --- Chaos: Artificial latency (200-400ms) ---
  int latency = CHAOS_LATENCY_MIN_MS +
                (rand() % (CHAOS_LATENCY_MAX_MS - CHAOS_LATENCY_MIN_MS + 1));
  std::cerr << "[CHAOS] Sleeping " << latency << "ms before send.\n";
  std::this_thread::sleep_for(std::chrono::milliseconds(latency));

  // --- Chaos: Drop 15% of packets ---
  if ((rand() % 100) < CHAOS_DROP_PERCENT)
  {
    std::cerr << "[CHAOS] Dropping packet (seq="
              << packet.header.seq_num << ").\n";
    return;
  }

  // --- Chaos: Corrupt 5% of payloads ---
  if (payload_len > 0 && (rand() % 100) < CHAOS_CORRUPT_PERCENT)
  {
    std::cerr << "[CHAOS] Flipping a bit in payload (seq="
              << packet.header.seq_num << ").\n";
    sendBuf[HEADER_SIZE] ^= 0x01; // Flip least-significant bit of first byte
  }
#endif

  // sendto() shoots this flat buffer as a UDP postcard to destAddr
  int totalSize = HEADER_SIZE + payload_len;
  int sent = sendto(udpSocket, sendBuf, totalSize, 0, (sockaddr *)&destAddr,
                    sizeof(destAddr));

  if (sent == SOCKET_ERROR)
  {
    std::cerr << "[Sender] sendto() failed: " << WSAGetLastError() << std::endl;
  }
}

// PRIVATE HELPER: Polls for ACKs (blocking or non-blocking) and handles retransmissions
void RdtSender::pollAcksAndRetransmit(bool blocking)
{
  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(udpSocket, &readfds);

  struct timeval tv;
  if (blocking)
  {
    tv.tv_sec = 0;
    tv.tv_usec = 10000; // 10ms blocking wait
  }
  else
  {
    tv.tv_sec = 0;
    tv.tv_usec = 0; // Instant return
  }

  // Use select to check for incoming ACKs
  int ready = select(0, &readfds, NULL, NULL, &tv);
  if (ready > 0 && FD_ISSET(udpSocket, &readfds))
  {
    char recvBuf[HEADER_SIZE + MAX_PAYLOAD];
    sockaddr_in fromAddr{};
    int fromLen = sizeof(fromAddr);

    int n = recvfrom(udpSocket, recvBuf, sizeof(recvBuf), 0, (sockaddr *)&fromAddr, &fromLen);
    if (n >= HEADER_SIZE)
    {
      char checksumBuf[HEADER_SIZE + MAX_PAYLOAD];
      memcpy(checksumBuf, recvBuf, n);
      checksumBuf[13] = 0;
      checksumBuf[14] = 0;

      uint16_t computed = internetChecksum((const uint8_t *)checksumBuf, n);
      uint16_t received_checksum;
      memcpy(&received_checksum, recvBuf + 13, 2);
      received_checksum = ntohs(received_checksum);

      if (computed == received_checksum)
      {
        RdtHeader ackHeader = deserializeHeader(recvBuf);
        if (ackHeader.flags & FLAG_ACK)
        {
          // Find the packet in the window and mark it acked
          for (auto &pkt : window)
          {
            if (pkt.seq_num == ackHeader.ack_num)
            {
              pkt.acked = true;
              break;
            }
          }
          // Slide the window forward if the base is acked
          while (!window.empty() && window.front().acked)
          {
            window.pop_front();
          }
        }
      }
    }
  }

  // Check for retransmissions
  auto now = std::chrono::steady_clock::now();
  for (auto &pkt : window)
  {
    if (!pkt.acked)
    {
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - pkt.sent_time).count();
      if (duration > timeoutMs)
      {
        // Retransmit
        std::cerr << "[Sender] Timeout for seq=" << pkt.seq_num << ", retransmitting!" << std::endl;

        RdtPacket rawPkt;
        memset(&rawPkt, 0, sizeof(rawPkt));
        rawPkt.header.seq_num = pkt.seq_num;
        rawPkt.header.ack_num = 0;
        rawPkt.header.flags = FLAG_DATA;
        rawPkt.header.window_size = 10;
        rawPkt.header.payload_len = static_cast<uint16_t>(pkt.data.size());
        rawPkt.header.reserved = 0;
        rawPkt.header.checksum = 0;
        memcpy(rawPkt.payload, pkt.data.data(), pkt.data.size());

        char tempBuf[HEADER_SIZE + MAX_PAYLOAD];
        memset(tempBuf, 0, sizeof(tempBuf));
        serializeHeader(rawPkt.header, tempBuf);
        memcpy(tempBuf + HEADER_SIZE, rawPkt.payload, pkt.data.size());
        rawPkt.header.checksum = internetChecksum((const uint8_t *)tempBuf, HEADER_SIZE + pkt.data.size());

        sendRawPacket(rawPkt);
        pkt.sent_time = now; // Reset timer
      }
    }
  }
}

// PUBLIC: High-level Selective Repeat send
bool RdtSender::sendChunk(uint32_t seqNum, const char *data, size_t len)
{
  const int WINDOW_SIZE = 10;

  if (len > MAX_PAYLOAD)
  {
    std::cerr << "[Sender] REJECTED: chunk length " << len
              << " exceeds MAX_PAYLOAD (" << MAX_PAYLOAD << ")." << std::endl;
    return false;
  }

  // --- Build the packet ---
  // RdtPacket packet;
  // memset(&packet, 0, sizeof(packet));

  // 1. If window is full, block until space frees up
  while (window.size() >= WINDOW_SIZE)
  {
    pollAcksAndRetransmit(true);
  }

  // 2. Add packet to window
  InFlightPacket pkt;
  pkt.seq_num = seqNum;
  pkt.data.assign(data, data + len);
  pkt.sent_time = std::chrono::steady_clock::now();
  pkt.acked = false;
  window.push_back(pkt);

  // 3. Send the packet
  RdtPacket rawPkt;
  memset(&rawPkt, 0, sizeof(rawPkt));
  rawPkt.header.seq_num = seqNum;
  rawPkt.header.ack_num = 0;
  rawPkt.header.flags = FLAG_DATA;
  rawPkt.header.window_size = WINDOW_SIZE;
  rawPkt.header.payload_len = static_cast<uint16_t>(len);
  rawPkt.header.reserved = 0;
  rawPkt.header.checksum = 0;
  memcpy(rawPkt.payload, data, len);

  char tempBuf[HEADER_SIZE + MAX_PAYLOAD];
  memset(tempBuf, 0, sizeof(tempBuf));
  serializeHeader(rawPkt.header, tempBuf);
  memcpy(tempBuf + HEADER_SIZE, rawPkt.payload, len);
  rawPkt.header.checksum = internetChecksum((const uint8_t *)tempBuf, HEADER_SIZE + len);

  std::cout << "[Sender] Sending seq=" << seqNum << std::endl;
  sendRawPacket(rawPkt);

  // 4. Quickly check for ACKs before returning
  pollAcksAndRetransmit(false);

  return true;
}

// PUBLIC: Flush remaining in-flight packets
bool RdtSender::flush()
{
  while (!window.empty())
  {
    pollAcksAndRetransmit(true);
  }
  return true;
}

bool RdtSender::receiveNext(uint32_t &outSeqNum, std::vector<char> &outData, bool &outIsFinal)
{
  // RdtSender does not receive data chunks.
  return false;
}

bool RdtSender::waitForClientReady()
{
  std::cout << "[Sender] Waiting for client READY (SYN) packet on PASV port..." << std::endl;
  char recvBuf[HEADER_SIZE + MAX_PAYLOAD];
  sockaddr_in clientAddr{};
  int clientLen = sizeof(clientAddr);

  while (true)
  {
    int n = recvfrom(udpSocket, recvBuf, sizeof(recvBuf), 0,
                     (sockaddr *)&clientAddr, &clientLen);

    if (n == SOCKET_ERROR)
    {
      int err = WSAGetLastError();
      if (err == WSAETIMEDOUT)
      {
        // Just keep waiting if it times out
        continue;
      }
      std::cerr << "[Sender] recvfrom() error: " << err << std::endl;
      return false;
    }

    if (n < HEADER_SIZE)
      continue;

    // Verify Checksum
    char checksumBuf[HEADER_SIZE + MAX_PAYLOAD];
    memcpy(checksumBuf, recvBuf, n);
    checksumBuf[13] = 0;
    checksumBuf[14] = 0;

    uint16_t computed = internetChecksum((const uint8_t *)checksumBuf, n);
    uint16_t received_checksum;
    memcpy(&received_checksum, recvBuf + 13, 2);
    received_checksum = ntohs(received_checksum);

    if (computed != received_checksum)
      continue;

    RdtHeader header = deserializeHeader(recvBuf);

    if (header.flags & FLAG_SYN)
    {
      destAddr = clientAddr;
      std::cout << "[Sender] Received SYN packet! Client address captured." << std::endl;

      // Send ACK for the SYN
      RdtHeader ackHeader{};
      ackHeader.seq_num = 0;
      ackHeader.ack_num = header.seq_num;
      ackHeader.flags = FLAG_ACK;
      ackHeader.window_size = 1;
      ackHeader.payload_len = 0;
      ackHeader.reserved = 0;
      ackHeader.checksum = 0;

      char ackBuf[HEADER_SIZE];
      serializeHeader(ackHeader, ackBuf);
      ackHeader.checksum = internetChecksum((const uint8_t *)ackBuf, HEADER_SIZE);
      serializeHeader(ackHeader, ackBuf);

      sendto(udpSocket, ackBuf, HEADER_SIZE, 0, (sockaddr *)&destAddr, sizeof(destAddr));

      return true;
    }
  }
}
