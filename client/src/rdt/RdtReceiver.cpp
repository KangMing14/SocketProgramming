#include "RdtReceiver.h"
#include <cstring>
#include <iostream>

namespace hybridftp::client {

namespace {
bool sameEndpoint(const sockaddr_in& left, const sockaddr_in& right) {
  return left.sin_family == right.sin_family &&
         left.sin_port == right.sin_port &&
         left.sin_addr.s_addr == right.sin_addr.s_addr;
}

bool validChecksum(const char* bytes, int length) {
  if (length < static_cast<int>(HEADER_SIZE)) return false;
  char copy[HEADER_SIZE + MAX_PAYLOAD];
  memcpy(copy, bytes, length);
  copy[13] = 0;
  copy[14] = 0;
  uint16_t received = 0;
  memcpy(&received, bytes + 13, sizeof(received));
  received = ntohs(received);
  return internetChecksum(reinterpret_cast<const uint8_t*>(copy), length) == received;
}
}

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
  DWORD timeout = DATA_IDLE_TIMEOUT_MS;
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
  DWORD timeout = DATA_IDLE_TIMEOUT_MS;
  setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout,
             sizeof(timeout));
  std::cout << "[Receiver] Initialized with existing PASV socket." << std::endl;
}

void RdtReceiver::expectPeerIp(const in_addr& address) {
  expectedPeerIp = address;
  expectedPeerIpSet = true;
}

bool RdtReceiver::signalClientReady(const sockaddr_in& serverDataAddress) {
  if (!isValid()) return false;
  peerAddr = serverDataAddress;
  peerKnown = true;

  RdtHeader syn{};
  syn.seq_num = 0;
  syn.flags = FLAG_SYN;
  syn.window_size = 1;
  char synBuffer[HEADER_SIZE]{};
  serializeHeader(syn, synBuffer);
  syn.checksum = internetChecksum(
      reinterpret_cast<const uint8_t*>(synBuffer), HEADER_SIZE);
  serializeHeader(syn, synBuffer);

  DWORD timeout = HANDSHAKE_TIMEOUT_MS;
  setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&timeout), sizeof(timeout));
  for (int attempt = 0; attempt < HANDSHAKE_MAX_RETRIES; ++attempt) {
    if (sendto(udpSocket, synBuffer, static_cast<int>(HEADER_SIZE), 0,
               reinterpret_cast<const sockaddr*>(&peerAddr),
               sizeof(peerAddr)) == SOCKET_ERROR) {
      return false;
    }

    char ackBuffer[HEADER_SIZE + MAX_PAYLOAD];
    sockaddr_in from{};
    int fromLength = sizeof(from);
    const int received = recvfrom(
        udpSocket, ackBuffer, sizeof(ackBuffer), 0,
        reinterpret_cast<sockaddr*>(&from), &fromLength);
    if (received == SOCKET_ERROR) {
      if (WSAGetLastError() == WSAETIMEDOUT) continue;
      return false;
    }
    if (!sameEndpoint(from, peerAddr) || !validChecksum(ackBuffer, received)) {
      continue;
    }
    const RdtHeader ack = deserializeHeader(ackBuffer);
    if ((ack.flags & FLAG_ACK) && ack.ack_num == syn.seq_num) {
      DWORD idleTimeout = DATA_IDLE_TIMEOUT_MS;
      setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&idleTimeout),
                 sizeof(idleTimeout));
      return true;
    }
  }
  return false;
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
  auto buffered = outOfOrderBuffer.find(expectedSequence);
  if (buffered != outOfOrderBuffer.end()) {
    outSeqNum = expectedSequence++;
    outData = std::move(buffered->second.first);
    outIsFinal = buffered->second.second;
    outOfOrderBuffer.erase(buffered);
    return true;
  }

  char recvBuf[HEADER_SIZE + MAX_PAYLOAD];
  sockaddr_in clientAddr{};
  int clientLen = sizeof(clientAddr);

  while (true) {
    // Block here until a UDP packet arrives (or the timeout fires)
    int n = recvfrom(udpSocket, recvBuf, sizeof(recvBuf), 0,
                     (sockaddr *)&clientAddr, &clientLen);

    if (n == SOCKET_ERROR) {
      int err = WSAGetLastError();
      if (err == WSAETIMEDOUT) {
        std::cerr << "[Receiver] Timeout — no data received." << std::endl;
      } else {
        std::cerr << "[Receiver] recvfrom() error: " << err << std::endl;
      }
      return false;
    }

    if (n < static_cast<int>(HEADER_SIZE)) {
      std::cerr << "[Receiver] Packet too small, ignoring." << std::endl;
      continue;
    }

    if (!validChecksum(recvBuf, n)) {
      // SILENT DROP — do NOT send any reply. The sender's timer will
      // retransmit.
      std::cerr << "[Receiver] Checksum MISMATCH — packet corrupted, dropping "
                   "silently."
                << std::endl;
      continue; // Loop and wait for the retransmission
    }

    // --- Checksum passed — Deserialize the header ---
    RdtHeader header = deserializeHeader(recvBuf);

    // Make sure this is a DATA packet (not some stray ACK we accidentally
    // received)
    if (!(header.flags & FLAG_DATA)) {
      std::cerr << "[Receiver] Packet is not a DATA packet, ignoring."
                << std::endl;
      continue;
    }
    if (expectedPeerIpSet &&
        clientAddr.sin_addr.s_addr != expectedPeerIp.s_addr) {
      continue;
    }
    if (peerKnown && !sameEndpoint(clientAddr, peerAddr)) continue;
    if (!peerKnown) {
      peerAddr = clientAddr;
      peerKnown = true;
    }

    // --- Extract the payload from the buffer ---
    uint16_t payload_len = header.payload_len;
    if (payload_len > MAX_PAYLOAD)
      payload_len = MAX_PAYLOAD;

    int actual_payload = n - HEADER_SIZE;
    if (payload_len > actual_payload)
      payload_len = actual_payload;

    sendAck(header.seq_num, clientAddr);

    if (header.seq_num < expectedSequence) continue;

    std::vector<char> payload(payload_len);
    if (payload_len > 0) {
      memcpy(payload.data(), recvBuf + HEADER_SIZE, payload_len);
    }
    const bool isFinal = (header.flags & FLAG_FIN) != 0;
    if (header.seq_num > expectedSequence) {
      if (header.seq_num <= expectedSequence + 10) {
        outOfOrderBuffer.emplace(
            header.seq_num, std::make_pair(std::move(payload), isFinal));
      }
      continue;
    }

    outSeqNum = expectedSequence++;
    outData = std::move(payload);
    outIsFinal = isFinal;
    return true;
  }
}

bool RdtReceiver::sendChunk(uint32_t, const char*, size_t, bool) {
  // RdtReceiver does not send data chunks.
  return false;
}

}
