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
#define CHAOS_DROP_PERCENT   15   // Drop 15% of outgoing packets
#define CHAOS_CORRUPT_PERCENT 5   // Flip a bit in 5% of payloads
#define CHAOS_LATENCY_MIN_MS 200  // Minimum artificial latency in ms
#define CHAOS_LATENCY_MAX_MS 400  // Maximum artificial latency in ms
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
  if ((rand() % 100) < CHAOS_DROP_PERCENT) {
    std::cerr << "[CHAOS] Dropping packet (seq="
              << packet.header.seq_num << ").\n";
    return;
  }

  // --- Chaos: Corrupt 5% of payloads ---
  if (payload_len > 0 && (rand() % 100) < CHAOS_CORRUPT_PERCENT) {
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

// PRIVATE HELPER: Waits for an ACK packet matching the expected sequence number
// Returns true if a matching ACK arrived, false if timeout or wrong ACK
bool RdtSender::waitForAck(uint32_t expected_ack_num)
{
  while (true)
  {
    char recvBuf[HEADER_SIZE + MAX_PAYLOAD];
    sockaddr_in fromAddr{};
    int fromLen = sizeof(fromAddr);

    // recvfrom() blocks here until a packet arrives OR the SO_RCVTIMEO timeout
    // fires
    int n = recvfrom(udpSocket, recvBuf, sizeof(recvBuf), 0,
                     (sockaddr *)&fromAddr, &fromLen);

    if (n == SOCKET_ERROR)
    {
      int err = WSAGetLastError();
      if (err == WSAETIMEDOUT)
      {
        std::cerr << "[Sender] Timeout waiting for ACK " << expected_ack_num
                  << " — will retransmit." << std::endl;
      }
      return false; // Real timeout, return false so sendChunk will retransmit
    }

    if (n < HEADER_SIZE)
    {
      std::cerr << "[Sender] Packet too small, ignoring." << std::endl;
      continue;
    }

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
    {
      std::cerr << "[Sender] Checksum MISMATCH on ACK, ignoring." << std::endl;
      continue;
    }

    // Deserialize the received bytes back into a header struct
    RdtHeader ackHeader = deserializeHeader(recvBuf);

    // Golden Rule: Verify this is actually a proper ACK with FLAG_ACK set
    if (!(ackHeader.flags & FLAG_ACK))
    {
      std::cerr << "[Sender] Received packet is not an ACK, ignoring."
                << std::endl;
      continue;
    }

    // Check if the ACK matches what we were waiting for
    if (ackHeader.ack_num == expected_ack_num)
    {
      return true; // Success!
    }

    // We got an ACK, but for the wrong packet (stale ACK from a previous
    // retransmit)
    std::cerr << "[Sender] Stale ACK received (got " << ackHeader.ack_num
              << " expected " << expected_ack_num << "), ignoring." << std::endl;
  }
}

// PUBLIC: High-level Stop-and-Wait send
// Builds the packet, sends it, and retransmits on timeout until ACKed
bool RdtSender::sendChunk(uint32_t seqNum, const char* data, size_t len)
{
  // --- Build the packet ---
  RdtPacket packet;
  memset(&packet, 0, sizeof(packet));

  packet.header.seq_num = seqNum;
  packet.header.ack_num = 0;
  packet.header.flags = FLAG_DATA;
  packet.header.window_size = 1; // Stop-and-Wait: window of 1
  packet.header.payload_len = static_cast<uint16_t>(len);
  packet.header.reserved = 0;
  packet.header.checksum = 0; // Zero out before calculating

  // Copy the file data into the payload slot
  memcpy(packet.payload, data, len);

  // --- Calculate Checksum over the entire packet (header + payload) ---
  // First serialize the header with checksum=0 into a temp buffer
  char tempBuf[HEADER_SIZE + MAX_PAYLOAD];
  memset(tempBuf, 0, sizeof(tempBuf));
  serializeHeader(packet.header, tempBuf);
  memcpy(tempBuf + HEADER_SIZE, packet.payload, len);

  // Compute checksum over [header bytes + payload bytes]
  packet.header.checksum =
      internetChecksum((const uint8_t *)tempBuf, HEADER_SIZE + len);

  // --- Stop-and-Wait Retry Loop ---
  for (int attempt = 0; attempt < MAX_RETRIES; attempt++)
  {
    std::cout << "[Sender] Sending seq=" << seqNum << " (attempt "
              << attempt + 1 << "/" << MAX_RETRIES << ")" << std::endl;

    sendRawPacket(packet);

    if (waitForAck(seqNum))
    {
      std::cout << "[Sender] ACK received for seq=" << seqNum << std::endl;
      return true; // Successfully delivered!
    }
    // If we got here, the ACK didn't come in time — loop and retransmit
  }

  std::cerr << "[Sender] FAILED to deliver seq=" << seqNum << " after "
            << MAX_RETRIES << " attempts." << std::endl;
  return false;
}

bool RdtSender::receiveNext(uint32_t& outSeqNum, std::vector<char>& outData, bool& outIsFinal) {
    // RdtSender does not receive data chunks.
    return false;
}

bool RdtSender::waitForClientReady() {
  std::cout << "[Sender] Waiting for client READY (SYN) packet on PASV port..." << std::endl;
  char recvBuf[HEADER_SIZE + MAX_PAYLOAD];
  sockaddr_in clientAddr{};
  int clientLen = sizeof(clientAddr);

  while (true) {
    int n = recvfrom(udpSocket, recvBuf, sizeof(recvBuf), 0,
                     (sockaddr *)&clientAddr, &clientLen);

    if (n == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err == WSAETIMEDOUT) {
        // Just keep waiting if it times out
        continue;
      }
      std::cerr << "[Sender] recvfrom() error: " << err << std::endl;
      return false;
    }

    if (n < HEADER_SIZE) continue;

    // Verify Checksum
    char checksumBuf[HEADER_SIZE + MAX_PAYLOAD];
    memcpy(checksumBuf, recvBuf, n);
    checksumBuf[13] = 0;
    checksumBuf[14] = 0;

    uint16_t computed = internetChecksum((const uint8_t *)checksumBuf, n);
    uint16_t received_checksum;
    memcpy(&received_checksum, recvBuf + 13, 2);
    received_checksum = ntohs(received_checksum);

    if (computed != received_checksum) continue;

    RdtHeader header = deserializeHeader(recvBuf);

    if (header.flags & FLAG_SYN) {
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
