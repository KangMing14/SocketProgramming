#include "RdtSender.h"
#include <cstring>
#include <iostream>

#define MAX_RETRIES 10
#define HEADER_SIZE 16

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

  // Serialize the header (with htonl/htons byte order conversion) into the
  // front
  serializeHeader(packet.header, sendBuf);

  // Copy the payload right after the header
  uint16_t payload_len = packet.header.payload_len;
  if (payload_len > MAX_PAYLOAD)
    payload_len = MAX_PAYLOAD;
  memcpy(sendBuf + HEADER_SIZE, packet.payload, payload_len);

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
    return false;
  }

  // Deserialize the received bytes back into a header struct
  RdtHeader ackHeader = deserializeHeader(recvBuf);

  // Golden Rule: Verify this is actually a proper ACK with FLAG_ACK set
  if (!(ackHeader.flags & FLAG_ACK))
  {
    std::cerr << "[Sender] Received packet is not an ACK, ignoring."
              << std::endl;
    return false;
  }

  // Check if the ACK matches what we were waiting for
  if (ackHeader.ack_num == expected_ack_num)
  {
    return true; //
  }

  // We got an ACK, but for the wrong packet (stale ACK from a previous
  // retransmit)
  std::cerr << "[Sender] Stale ACK received (got " << ackHeader.ack_num
            << " expected " << expected_ack_num << "), ignoring." << std::endl;
  return false;
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
