#include "RdtReceiver.h"

#include <chrono>
#include <cstring>
#include <iostream>
#include <utility>

namespace hybridftp::client {

namespace {
constexpr DWORD ABORT_POLL_TIMEOUT_MS = 100;

bool sameEndpoint(const sockaddr_in& left, const sockaddr_in& right) {
  return left.sin_family == right.sin_family &&
         left.sin_port == right.sin_port &&
         left.sin_addr.s_addr == right.sin_addr.s_addr;
}
}

bool RdtReceiver::isAbortRequested() const {
  return abortPredicate && abortPredicate();
}

void RdtReceiver::applyDataTimeout() {
  if (!isValid()) return;
  const DWORD timeout = abortPredicate ? ABORT_POLL_TIMEOUT_MS
                                       : DATA_IDLE_TIMEOUT_MS;
  setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

void RdtReceiver::setAbortPredicate(std::function<bool()> predicate) {
  abortPredicate = std::move(predicate);
  applyDataTimeout();
}

RdtReceiver::RdtReceiver(uint16_t listenPort) {
  udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (udpSocket == INVALID_SOCKET) {
    std::cerr << "[Receiver] Failed to create socket: " << WSAGetLastError()
              << std::endl;
    return;
  }

  const DWORD timeout = DATA_IDLE_TIMEOUT_MS;
  if (setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout)) ==
      SOCKET_ERROR) {
    std::cerr << "[Receiver] setsockopt() failed: " << WSAGetLastError()
              << std::endl;
    closesocket(udpSocket);
    udpSocket = INVALID_SOCKET;
    return;
  }

  localAddr.sin_family = AF_INET;
  localAddr.sin_port = htons(listenPort);
  localAddr.sin_addr.s_addr = INADDR_ANY;
  if (bind(udpSocket, reinterpret_cast<sockaddr*>(&localAddr),
           sizeof(localAddr)) == SOCKET_ERROR) {
    std::cerr << "[Receiver] bind() failed: " << WSAGetLastError() << std::endl;
    closesocket(udpSocket);
    udpSocket = INVALID_SOCKET;
    return;
  }

  std::cout << "[Receiver] Listening on port " << listenPort << "..."
            << std::endl;
}

RdtReceiver::RdtReceiver(SOCKET existingSocket)
    : udpSocket(existingSocket) {
  if (!isValid()) return;
  const DWORD timeout = DATA_IDLE_TIMEOUT_MS;
  if (setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout)) ==
      SOCKET_ERROR) {
    closesocket(udpSocket);
    udpSocket = INVALID_SOCKET;
    return;
  }
  std::cout << "[Receiver] Initialized with existing PASV socket." << std::endl;
}

void RdtReceiver::expectPeerIp(const in_addr& address) {
  expectedPeerIp = address;
  expectedPeerIpSet = true;
}

bool RdtReceiver::signalClientReady(const sockaddr_in& serverDataAddress) {
  if (!isValid() || isAbortRequested()) return false;
  peerAddr = serverDataAddress;
  peerKnown = true;

  RdtHeader syn{};
  syn.seq_num = 0;
  syn.flags = FLAG_SYN;
  syn.window_size = static_cast<std::uint16_t>(RDT_RECEIVE_WINDOW);
  char synBuffer[HEADER_SIZE]{};
  serializeHeader(syn, synBuffer);
  syn.checksum = internetChecksum(
      reinterpret_cast<const uint8_t*>(synBuffer), HEADER_SIZE);
  serializeHeader(syn, synBuffer);

  const DWORD timeout = HANDSHAKE_TIMEOUT_MS;
  if (setsockopt(udpSocket, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout)) ==
      SOCKET_ERROR) {
    return false;
  }

  for (int attempt = 0; attempt < HANDSHAKE_MAX_RETRIES; ++attempt) {
    if (isAbortRequested()) {
      applyDataTimeout();
      return false;
    }
    if (sendto(udpSocket, synBuffer, static_cast<int>(HEADER_SIZE), 0,
               reinterpret_cast<const sockaddr*>(&peerAddr),
               sizeof(peerAddr)) == SOCKET_ERROR) {
      applyDataTimeout();
      return false;
    }

    char responseBuffer[HEADER_SIZE + MAX_PAYLOAD];
    sockaddr_in from{};
    int fromLength = sizeof(from);
    const int received = recvfrom(
        udpSocket, responseBuffer, sizeof(responseBuffer), 0,
        reinterpret_cast<sockaddr*>(&from), &fromLength);
    if (received == SOCKET_ERROR) {
      if (isAbortRequested()) {
        applyDataTimeout();
        return false;
      }
      if (WSAGetLastError() == WSAETIMEDOUT) continue;
      applyDataTimeout();
      return false;
    }

    RdtHeader response{};
    if (!sameEndpoint(from, peerAddr) ||
        !decodeValidatedDatagram(
            responseBuffer, static_cast<std::size_t>(received), response)) {
      continue;
    }
    if ((response.flags & FLAG_ACK) && !(response.flags & FLAG_DATA) &&
        response.payload_len == 0 && response.ack_num == syn.seq_num) {
      applyDataTimeout();
      return true;
    }
    if (response.flags & FLAG_DATA) {
      pendingDatagram.assign(responseBuffer, responseBuffer + received);
      pendingFrom = from;
      applyDataTimeout();
      return true;
    }
  }

  applyDataTimeout();
  return false;
}

RdtReceiver::~RdtReceiver() {
  if (udpSocket != INVALID_SOCKET) closesocket(udpSocket);
}

void RdtReceiver::sendAck(uint32_t ackNumber, sockaddr_in& clientAddress) {
  RdtHeader acknowledgement{};
  acknowledgement.ack_num = ackNumber;
  acknowledgement.flags = FLAG_ACK;
  acknowledgement.window_size =
      static_cast<std::uint16_t>(RDT_RECEIVE_WINDOW);

  char bytes[HEADER_SIZE]{};
  serializeHeader(acknowledgement, bytes);
  acknowledgement.checksum = internetChecksum(
      reinterpret_cast<const uint8_t*>(bytes), HEADER_SIZE);
  serializeHeader(acknowledgement, bytes);
  sendto(udpSocket, bytes, static_cast<int>(HEADER_SIZE), 0,
         reinterpret_cast<sockaddr*>(&clientAddress), sizeof(clientAddress));
}

bool RdtReceiver::receiveNext(uint32_t& outSequence,
                              std::vector<char>& outData,
                              bool& outIsFinal) {
  if (isAbortRequested()) return false;
  const auto idleDeadline = std::chrono::steady_clock::now() +
      std::chrono::milliseconds(DATA_IDLE_TIMEOUT_MS);

  const auto buffered = outOfOrderBuffer.find(expectedSequence);
  if (buffered != outOfOrderBuffer.end()) {
    outSequence = expectedSequence++;
    outData = std::move(buffered->second.first);
    outIsFinal = buffered->second.second;
    outOfOrderBuffer.erase(buffered);
    return true;
  }

  char receiveBuffer[HEADER_SIZE + MAX_PAYLOAD];
  sockaddr_in senderAddress{};
  int senderLength = sizeof(senderAddress);

  while (true) {
    if (isAbortRequested()) return false;
    int received = 0;
    if (!pendingDatagram.empty()) {
      received = static_cast<int>(pendingDatagram.size());
      std::memcpy(receiveBuffer, pendingDatagram.data(), pendingDatagram.size());
      senderAddress = pendingFrom;
      pendingDatagram.clear();
    } else {
      received = recvfrom(
          udpSocket, receiveBuffer, sizeof(receiveBuffer), 0,
          reinterpret_cast<sockaddr*>(&senderAddress), &senderLength);
    }

    if (received == SOCKET_ERROR) {
      const int error = WSAGetLastError();
      if (error == WSAETIMEDOUT) {
        if (isAbortRequested()) return false;
        if (abortPredicate && std::chrono::steady_clock::now() < idleDeadline) {
          continue;
        }
      }
      return false;
    }

    RdtHeader header{};
    if (!decodeValidatedDatagram(
            receiveBuffer, static_cast<std::size_t>(received), header) ||
        !(header.flags & FLAG_DATA)) {
      continue;
    }
    if (expectedPeerIpSet &&
        senderAddress.sin_addr.s_addr != expectedPeerIp.s_addr) {
      continue;
    }
    if (peerKnown && !sameEndpoint(senderAddress, peerAddr)) continue;
    if (!peerKnown) {
      peerAddr = senderAddress;
      peerKnown = true;
    }

    if (header.seq_num < expectedSequence) {
      sendAck(header.seq_num, senderAddress);
      continue;
    }
    const std::uint64_t windowEnd =
        static_cast<std::uint64_t>(expectedSequence) + RDT_RECEIVE_WINDOW;
    if (static_cast<std::uint64_t>(header.seq_num) >= windowEnd) {
      continue;
    }

    std::vector<char> payload(header.payload_len);
    if (header.payload_len > 0) {
      std::memcpy(payload.data(), receiveBuffer + HEADER_SIZE,
                  header.payload_len);
    }
    sendAck(header.seq_num, senderAddress);
    const bool isFinal = (header.flags & FLAG_FIN) != 0;

    if (header.seq_num > expectedSequence) {
      outOfOrderBuffer.emplace(
          header.seq_num, std::make_pair(std::move(payload), isFinal));
      continue;
    }

    outSequence = expectedSequence++;
    outData = std::move(payload);
    outIsFinal = isFinal;
    return true;
  }
}

bool RdtReceiver::sendChunk(uint32_t, const char*, size_t, bool) {
  return false;
}

}
