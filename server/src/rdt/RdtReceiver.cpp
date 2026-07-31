#include "RdtReceiver.h"
#include <cstring>
#include <iostream>

#include "../common/ProtocolConstants.h"

// Constructor: creates a UDP socket and BINDS it to a port to listen
RdtReceiver::RdtReceiver(uint16_t listenPort) {
  // Step 1: Create a UDP socket — same as sender
  udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udpSocket == INVALID_SOCKET) {
    std::cerr << "[Receiver] Failed to create socket: " << WSAGetLastError()
              << std::endl;
    return;
  }

  // Step 2: Set the timeout so receiveData() doesn't block forever
  DWORD timeout = 5000; // 5-second receive timeout
  setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
             sizeof(timeout));

  // Step 3: BIND the socket to the listen port
  // This is the KEY difference from the sender — the receiver must nail down a
  // port so the OS knows "any UDP packets arriving on this port belong to this
  // socket"
  memset(&localAddr, 0, sizeof(localAddr));
  localAddr.sin_family = AF_INET;
  localAddr.sin_port = htons(listenPort);
  localAddr.sin_addr.s_addr = INADDR_ANY; // Accept from any IP address

  if (bind(udpSocket, (sockaddr *)&localAddr, sizeof(localAddr)) ==
      SOCKET_ERROR) {
    std::cerr << "[Receiver] bind() failed: " << WSAGetLastError() << std::endl;
    closesocket(udpSocket);
    udpSocket = INVALID_SOCKET;
    return;
  }

  std::cout << "[Receiver] Listening on port " << listenPort << "..."
            << std::endl;
}

// Constructor for PASV mode: reuses an already-bound socket owned by the
// server's PassiveModeHandler. Does NOT call socket() or bind().
RdtReceiver::RdtReceiver(SOCKET existingSocket)
    : udpSocket(existingSocket) {
  // Just apply the timeout — socket is already created and bound by caller.
  DWORD timeout = 5000;
  setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
             sizeof(timeout));
  std::cout << "[Receiver] Initialized with existing PASV socket." << std::endl;
}

// Destructor: close the socket
RdtReceiver::~RdtReceiver() {
  if (udpSocket != INVALID_SOCKET) {
    closesocket(udpSocket);
  }
}

// PRIVATE HELPER: Sends a tiny ACK packet back to the sender's address
void RdtReceiver::sendAck(uint32_t ack_num, sockaddr_in &clientAddr) {
  // Build a minimal ACK packet (header only, no payload needed)
  RdtHeader ackHeader{};
  ackHeader.seq_num = 0;
  ackHeader.ack_num = ack_num; // Tell the sender which packet we are ACKing
  ackHeader.flags = FLAG_ACK;  // Golden Rule: Set the FLAG_ACK bit
  ackHeader.window_size = 1;
  ackHeader.payload_len = 0;
  ackHeader.reserved = 0;
  ackHeader.checksum = 0;

  // Compute checksum on the header alone
  char ackBuf[HEADER_SIZE];
  serializeHeader(ackHeader, ackBuf);
  ackHeader.checksum = internetChecksum((const uint8_t *)ackBuf, HEADER_SIZE);

  // Serialize again with the real checksum
  serializeHeader(ackHeader, ackBuf);

  // Send the ACK back to the exact address the data came from
  sendto(udpSocket, ackBuf, HEADER_SIZE, 0, (sockaddr *)&clientAddr,
         sizeof(clientAddr));

  std::cout << "[Receiver] ACK sent for seq=" << ack_num << std::endl;
}

// PUBLIC: Wait for a valid data packet, verify it, and ACK it back
//
// Returns true on success, false on error/timeout.
// Golden Rules applied:
//   1. Always check checksum first — drop silently if bad
//   2. Always send ACK even for duplicates — but tell the caller it's a
//   duplicate
bool RdtReceiver::receiveNext(uint32_t &outSeqNum, std::vector<char> &outData,
                              bool &outIsFinal) {
  const int WINDOW_SIZE = 10;

  // 1. Check if the expected packet is already buffered
  auto it = outOfOrderBuffer.find(expected_seq);
  if (it != outOfOrderBuffer.end()) {
    outData = it->second.first;
    outIsFinal = it->second.second;
    outSeqNum = expected_seq;
    outOfOrderBuffer.erase(it);
    expected_seq++;
    return true;
  }

  char recvBuf[HEADER_SIZE + MAX_PAYLOAD];
  sockaddr_in clientAddr{};
  int clientLen = sizeof(clientAddr);

  while (true) {
    int n = recvfrom(udpSocket, recvBuf, sizeof(recvBuf), 0, (sockaddr *)&clientAddr, &clientLen);

    if (n == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err == WSAETIMEDOUT) {
        continue;
      }
      std::cerr << "[Receiver] recvfrom() error: " << err << std::endl;
      return false;
    }

    if (n < HEADER_SIZE) continue;

    char checksumBuf[HEADER_SIZE + MAX_PAYLOAD];
    memcpy(checksumBuf, recvBuf, n);
    checksumBuf[13] = 0;
    checksumBuf[14] = 0;

    uint16_t computed = internetChecksum((const uint8_t *)checksumBuf, n);
    uint16_t received_checksum;
    memcpy(&received_checksum, recvBuf + 13, 2);
    received_checksum = ntohs(received_checksum);

    if (computed != received_checksum) {
      std::cerr << "[Receiver] Checksum MISMATCH, dropping." << std::endl;
      continue;
    }

    RdtHeader header = deserializeHeader(recvBuf);
    if (!(header.flags & FLAG_DATA)) continue;

    // ALWAYS ACK DATA PACKETS IN SELECTIVE REPEAT
    sendAck(header.seq_num, clientAddr);

    uint16_t payload_len = header.payload_len;
    if (payload_len > MAX_PAYLOAD) payload_len = MAX_PAYLOAD;
    int actual_payload = n - HEADER_SIZE;
    if (payload_len > actual_payload) payload_len = actual_payload;

    if (header.seq_num == expected_seq) {
      // In-order packet
      outData.resize(payload_len);
      memcpy(outData.data(), recvBuf + HEADER_SIZE, payload_len);
      outSeqNum = header.seq_num;
      outIsFinal = (header.flags & FLAG_FIN) != 0;
      seen_seq_nums.insert(header.seq_num);
      expected_seq++;
      return true;
    } else if (header.seq_num > expected_seq) {
      // Out-of-order packet (future)
      // Enforce strict upper bound to prevent memory exhaustion
      if (header.seq_num <= expected_seq + WINDOW_SIZE) {
        if (seen_seq_nums.find(header.seq_num) == seen_seq_nums.end()) {
          std::vector<char> data(payload_len);
          memcpy(data.data(), recvBuf + HEADER_SIZE, payload_len);
          bool isFin = (header.flags & FLAG_FIN) != 0;
          outOfOrderBuffer[header.seq_num] = std::make_pair(data, isFin);
          seen_seq_nums.insert(header.seq_num);
        }
      } else {
        std::cerr << "[Receiver] Packet seq=" << header.seq_num << " is too far ahead (>" << expected_seq + WINDOW_SIZE << "), dropping." << std::endl;
      }
    } else {
      // Duplicate packet (past)
      // Already ACKed it above, just ignore
    }
  }
}

bool RdtReceiver::sendChunk(uint32_t seqNum, const char *data, size_t len) {
  // RdtReceiver does not send data chunks.
  return false;
}
