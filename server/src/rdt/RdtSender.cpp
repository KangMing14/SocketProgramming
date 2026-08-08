#include "RdtSender.h"
#include <cstring>
#include <iostream>
#include <cstdlib>
#include <chrono>
#include <thread>
#include <utility>
#include <algorithm>
#include <cmath>

#include "../common/ProtocolConstants.h"

#define MAX_RETRIES 10

#ifndef RDT_DEBUG
#define RDT_DEBUG 0
#endif

// ---- CHAOS MODE ----
// Set to 0 for normal operation and 1 for chaos testing.
#ifndef CHAOS_MODE
#define CHAOS_MODE 0
#endif

#if CHAOS_MODE
#define CHAOS_DROP_PERCENT 15    // Drop 15% of outgoing packets
#define CHAOS_CORRUPT_PERCENT 5  // Flip a bit in 5% of payloads
#define CHAOS_LATENCY_MIN_MS 200 // Minimum artificial latency in ms
#define CHAOS_LATENCY_MAX_MS 400 // Maximum artificial latency in ms
#endif

namespace {
bool sameEndpoint(const sockaddr_in& left, const sockaddr_in& right) {
  return left.sin_family == right.sin_family &&
         left.sin_port == right.sin_port &&
         left.sin_addr.s_addr == right.sin_addr.s_addr;
}
}

// Constructor: creates the UDP socket, sets timeout, and saves the destination
// address
RdtSender::RdtSender(const std::string &targetIp, uint16_t targetPort,
                     int initialTimeoutMs)
    : timeoutMs(std::clamp(initialTimeoutMs, MIN_TIMEOUT_MS, MAX_TIMEOUT_MS)),
      estimatedRttMs(timeoutMs),
      devRttMs(0.0),
      cwnd(INITIAL_CWND),
      cleanAcks(0)
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
RdtSender::RdtSender(SOCKET existingSocket, int initialTimeoutMs)
    : udpSocket(existingSocket),
      timeoutMs(std::clamp(initialTimeoutMs, MIN_TIMEOUT_MS, MAX_TIMEOUT_MS)),
      estimatedRttMs(timeoutMs),
      devRttMs(0.0),
      cwnd(INITIAL_CWND),
      cleanAcks(0)
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

double RdtSender::getCongestionWindow() const noexcept { return cwnd; }
int RdtSender::getTimeoutMs() const noexcept { return timeoutMs; }

bool RdtSender::isAbortRequested() const {
  return abortPredicate && abortPredicate();
}

void RdtSender::setAbortPredicate(std::function<bool()> predicate) {
  abortPredicate = std::move(predicate);
}
double RdtSender::getEstimatedRttMs() const noexcept { return estimatedRttMs; }
double RdtSender::getDevRttMs() const noexcept { return devRttMs; }
size_t RdtSender::getCleanAckCount() const noexcept { return cleanAcks; }

size_t RdtSender::effectiveWindowSize() const {
    return std::clamp<size_t>(
        static_cast<size_t>(std::floor(cwnd)), 1, RDT_RECEIVE_WINDOW);
}

void RdtSender::updateRtt(double sampleRttMs) {
    const double previousEstimatedRtt = estimatedRttMs;

    devRttMs =
        (1.0 - RTT_BETA) * devRttMs +
        RTT_BETA *
            std::abs(sampleRttMs - previousEstimatedRtt);

    estimatedRttMs =
        (1.0 - RTT_ALPHA) * estimatedRttMs +
        RTT_ALPHA * sampleRttMs;

    timeoutMs = std::clamp(
        static_cast<int>(std::ceil(
            estimatedRttMs + 4.0 * devRttMs
        )),
        MIN_TIMEOUT_MS,
        MAX_TIMEOUT_MS
    );
}

void RdtSender::applyCongestionDecrease() {
#if RDT_DEBUG
    double previousCwnd = cwnd;
#endif
    cwnd = (std::max)(MIN_CWND, cwnd / 2.0);
    cleanAcks = 0;

    timeoutMs = std::clamp(
        timeoutMs * 2,
        MIN_TIMEOUT_MS,
        MAX_TIMEOUT_MS
    );
#if RDT_DEBUG
    std::cerr << "[RDT] Congestion event: cwnd "
              << previousCwnd << " -> " << cwnd
              << ", RTO=" << timeoutMs << "ms\n";
#endif
}

// PRIVATE HELPER: Physically serialize and shoot one packet into the network
bool RdtSender::sendRawPacket(const RdtPacket &packet, std::chrono::steady_clock::time_point &outSendTime)
{
  // We need a flat byte buffer: [16-byte header][payload bytes]
  char sendBuf[HEADER_SIZE + MAX_PAYLOAD];
  memset(sendBuf, 0, sizeof(sendBuf));

  // Serialize the header (with htonl/htons byte order conversion) into the front
  serializeHeader(packet.header, sendBuf);

  // Copy the payload right after the header
  const uint16_t payload_len = packet.header.payload_len;
  if (payload_len > MAX_PAYLOAD) return false;
  memcpy(sendBuf + HEADER_SIZE, packet.payload, payload_len);

#if CHAOS_MODE
  // --- Chaos: Artificial latency (200-400ms) ---
  int latency = CHAOS_LATENCY_MIN_MS +
                (rand() % (CHAOS_LATENCY_MAX_MS - CHAOS_LATENCY_MIN_MS + 1));
#if RDT_DEBUG
    std::cerr << "[CHAOS] Sleeping " << latency << "ms before send.\n";
#endif
    std::this_thread::sleep_for(std::chrono::milliseconds(latency));
  
    // --- Chaos: Drop 15% of packets ---
    if ((rand() % 100) < CHAOS_DROP_PERCENT)
    {
#if RDT_DEBUG
      std::cerr << "[CHAOS] Dropping packet (seq="
                << packet.header.seq_num << ").\n";
#endif
      outSendTime = std::chrono::steady_clock::now();
      return true;
    }
  
    // --- Chaos: Corrupt 5% of payloads ---
    if (payload_len > 0 && (rand() % 100) < CHAOS_CORRUPT_PERCENT)
    {
#if RDT_DEBUG
      std::cerr << "[CHAOS] Flipping a bit in payload (seq="
                << packet.header.seq_num << ").\n";
#endif
      sendBuf[HEADER_SIZE] ^= 0x01; // Flip least-significant bit of first byte
    }
#endif
  
    // sendto() shoots this flat buffer as a UDP postcard to destAddr
    int totalSize = HEADER_SIZE + payload_len;

    if (injectedPreSendDelay) {
        injectedPreSendDelay();
    }

    const auto sendTime = std::chrono::steady_clock::now();

    int sent;
    if (injectedSendTo) {
        sent = injectedSendTo(udpSocket, sendBuf, totalSize, 0, (sockaddr *)&destAddr, sizeof(destAddr));
    } else {
        sent = sendto(udpSocket, sendBuf, totalSize, 0, (sockaddr *)&destAddr, sizeof(destAddr));
    }
  
    if (sent == SOCKET_ERROR)
    {
      const int errorCode = WSAGetLastError();
      std::cerr << "[Sender] sendto() failed: " << errorCode << std::endl;
      return false;
    }

    outSendTime = sendTime;
    return true;
}

// PRIVATE HELPER: Polls for ACKs (blocking or non-blocking) and handles retransmissions
bool RdtSender::pollAcksAndRetransmit(bool blocking)
{
  if (isAbortRequested()) return false;
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

  while (true)
  {
    if (isAbortRequested()) return false;
    FD_ZERO(&readfds);
    FD_SET(udpSocket, &readfds);
    // Use select to check for incoming ACKs
    int ready = select(0, &readfds, NULL, NULL, &tv);
    if (ready > 0 && FD_ISSET(udpSocket, &readfds))
    {
      char recvBuf[HEADER_SIZE + MAX_PAYLOAD];
      sockaddr_in fromAddr{};
      int fromLen = sizeof(fromAddr);

      int n = recvfrom(udpSocket, recvBuf, sizeof(recvBuf), 0, (sockaddr *)&fromAddr, &fromLen);
      if (n < 0)
      {
        // Socket error (e.g. WSAECONNRESET from ICMP Port Unreachable).
        // Break out to prevent infinite select() loop on Windows.
        break;
      }

      RdtHeader ackHeader{};
      if (sameEndpoint(fromAddr, destAddr) &&
          decodeValidatedDatagram(
              recvBuf, static_cast<std::size_t>(n), ackHeader) &&
          (ackHeader.flags & FLAG_ACK) &&
          !(ackHeader.flags & FLAG_DATA) && ackHeader.payload_len == 0)
      {
            // Find the packet in the window and mark it acked
            for (auto &pkt : window)
            {
              if (pkt.seq_num == ackHeader.ack_num && !pkt.acked)
              {
                pkt.acked = true;

                if (!pkt.retransmitted) {
                    const auto ackTime = std::chrono::steady_clock::now();
                    const double sampleRttMs =
                        std::chrono::duration<double, std::milli>(
                            ackTime - pkt.sent_time
                        ).count();

                    updateRtt(sampleRttMs);
                    cleanAcks++;

                    if (cleanAcks >= effectiveWindowSize()) {
                        cwnd += 1.0;
                        cleanAcks = 0;
                    }

#if RDT_DEBUG
                    std::cout
                        << "[RDT] ACK seq=" << pkt.seq_num
                        << " SampleRTT=" << sampleRttMs << "ms"
                        << " EstimatedRTT=" << estimatedRttMs << "ms"
                        << " DevRTT=" << devRttMs << "ms"
                        << " RTO=" << timeoutMs << "ms"
                        << " cwnd=" << cwnd
                        << '\n';
#endif
                }
                break;
              }
            }
            // Slide the window forward if the base is acked
            while (!window.empty() && window.front().acked)
            {
              window.pop_front();
            }
      }
      
      // Force subsequent select calls in this loop to be non-blocking
      tv.tv_sec = 0;
      tv.tv_usec = 0;
    }
    else
    {
      break;
    }
  }

  // Check for retransmissions
  auto now = std::chrono::steady_clock::now();
  bool congestionEvent = false;
  int currentTimeoutMs = timeoutMs;

  for (auto &pkt : window)
  {
    if (isAbortRequested()) return false;
    if (!pkt.acked)
    {
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - pkt.sent_time).count();
      if (duration > currentTimeoutMs)
      {
        congestionEvent = true;
      }
    }
  }

  if (congestionEvent) {
    applyCongestionDecrease();
  }

  for (auto &pkt : window)
  {
    if (!pkt.acked)
    {
      auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - pkt.sent_time).count();
      if (duration > currentTimeoutMs)
      {
        if (pkt.retries >= MAX_RETRIES)
        {
          std::cerr << "[Sender] FATAL: seq=" << pkt.seq_num << " failed after " << MAX_RETRIES << " retries. Aborting." << std::endl;
          return false;
        }

        // Retransmit
        std::cerr << "[Sender] Timeout for seq=" << pkt.seq_num << ", retransmitting (attempt " << (pkt.retries + 1) << ")!" << std::endl;

        RdtPacket rawPkt;
        memset(&rawPkt, 0, sizeof(rawPkt));
        rawPkt.header.seq_num = pkt.seq_num;
        rawPkt.header.ack_num = 0;
        rawPkt.header.flags = FLAG_DATA | (pkt.is_final ? FLAG_FIN : 0);
        rawPkt.header.window_size = static_cast<uint16_t>(effectiveWindowSize());
        rawPkt.header.payload_len = static_cast<uint16_t>(pkt.data.size());
        rawPkt.header.reserved = 0;
        rawPkt.header.checksum = 0;
        memcpy(rawPkt.payload, pkt.data.data(), pkt.data.size());

        char tempBuf[HEADER_SIZE + MAX_PAYLOAD];
        memset(tempBuf, 0, sizeof(tempBuf));
        serializeHeader(rawPkt.header, tempBuf);
        memcpy(tempBuf + HEADER_SIZE, rawPkt.payload, pkt.data.size());
        rawPkt.header.checksum = internetChecksum((const uint8_t *)tempBuf, HEADER_SIZE + pkt.data.size());

        std::chrono::steady_clock::time_point actualSendTime;
        if (!sendRawPacket(rawPkt, actualSendTime)) {
            return false;
        }

        pkt.retries++;
        pkt.sent_time = actualSendTime;
        pkt.retransmitted = true;
      }
    }
  }
  return true;
}

// PUBLIC: High-level Selective Repeat send
bool RdtSender::sendChunk(uint32_t seqNum, const char *data, size_t len,
                          bool isFinal)
{
  if (isAbortRequested()) return false;
  if (len > MAX_PAYLOAD || (len > 0 && data == nullptr))
  {
    std::cerr << "[Sender] REJECTED: chunk length " << len
              << " exceeds MAX_PAYLOAD (" << MAX_PAYLOAD << ")." << std::endl;
    return false;
  }

  // 1. If window is full, block until space frees up
  while (window.size() >= effectiveWindowSize())
  {
    if (isAbortRequested()) return false;
    if (!pollAcksAndRetransmit(true)) return false;
  }

  // 2. Prepare the packet
  RdtPacket rawPkt;
  memset(&rawPkt, 0, sizeof(rawPkt));
  rawPkt.header.seq_num = seqNum;
  rawPkt.header.ack_num = 0;
  rawPkt.header.flags = FLAG_DATA | (isFinal ? FLAG_FIN : 0);
  rawPkt.header.window_size = static_cast<uint16_t>(effectiveWindowSize());
  rawPkt.header.payload_len = static_cast<uint16_t>(len);
  rawPkt.header.reserved = 0;
  rawPkt.header.checksum = 0;
  if (len > 0) memcpy(rawPkt.payload, data, len);

  char tempBuf[HEADER_SIZE + MAX_PAYLOAD];
  memset(tempBuf, 0, sizeof(tempBuf));
  serializeHeader(rawPkt.header, tempBuf);
  if (len > 0) memcpy(tempBuf + HEADER_SIZE, rawPkt.payload, len);
  rawPkt.header.checksum = internetChecksum((const uint8_t *)tempBuf, HEADER_SIZE + len);

#if RDT_DEBUG
  std::cout << "[Sender] Sending seq=" << seqNum << std::endl;
#endif

  std::chrono::steady_clock::time_point actualSendTime;
  if (!sendRawPacket(rawPkt, actualSendTime)) {
      return false;
  }

  // 3. Add packet to window
  InFlightPacket pkt;
  pkt.seq_num = seqNum;
  if (len > 0) pkt.data.assign(data, data + len);
  pkt.is_final = isFinal;
  pkt.sent_time = actualSendTime;
  pkt.acked = false;
  pkt.retransmitted = false;
  pkt.retries = 0;
  window.push_back(std::move(pkt));

  // 4. Quickly check for ACKs before returning
  if (!pollAcksAndRetransmit(false)) return false;

  return true;
}

// PUBLIC: Flush remaining in-flight packets
bool RdtSender::flush()
{
  while (!window.empty())
  {
    if (isAbortRequested()) return false;
    if (!pollAcksAndRetransmit(true)) return false;
  }
  return true;
}

bool RdtSender::receiveNext(uint32_t&, std::vector<char>&, bool&)
{
  // RdtSender does not receive data chunks.
  return false;
}

bool RdtSender::waitForClientReady()
{
  if (isAbortRequested()) return false;
  std::cout << "[Sender] Waiting for client READY (SYN) packet on PASV port..." << std::endl;
  char recvBuf[HEADER_SIZE + MAX_PAYLOAD];
  sockaddr_in clientAddr{};
  int clientLen = sizeof(clientAddr);

  DWORD handshakeTimeout = HANDSHAKE_TIMEOUT_MS;
  setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&handshakeTimeout),
             sizeof(handshakeTimeout));

  for (int attempt = 0; attempt < HANDSHAKE_MAX_RETRIES; ++attempt)
  {
    if (isAbortRequested()) return false;
    int n = recvfrom(udpSocket, recvBuf, sizeof(recvBuf), 0,
                     (sockaddr *)&clientAddr, &clientLen);

    if (n == SOCKET_ERROR)
    {
      int err = WSAGetLastError();
      if (isAbortRequested()) return false;
      if (err == WSAETIMEDOUT)
      {
        continue;
      }
      std::cerr << "[Sender] recvfrom() error: " << err << std::endl;
      return false;
    }

    RdtHeader header{};
    if (!decodeValidatedDatagram(
            recvBuf, static_cast<std::size_t>(n), header)) {
      continue;
    }

    if ((header.flags & FLAG_SYN) && !(header.flags & FLAG_DATA) &&
        header.payload_len == 0)
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

      return sendto(udpSocket, ackBuf, static_cast<int>(HEADER_SIZE), 0,
                    reinterpret_cast<sockaddr*>(&destAddr),
                    sizeof(destAddr)) != SOCKET_ERROR;
    }
  }

  std::cerr << "[Sender] Timed out waiting for client SYN." << std::endl;
  return false;
}
