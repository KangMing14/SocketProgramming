#include "CheckSum.h"
#include "ProtocolConstants.h"
#include "RdtReceiver.h"
#include "RdtSender.h"

#include <winsock2.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0,
                "WSAStartup failed");
    }
    ~WinsockSession() { WSACleanup(); }
};

SOCKET bindUdp(sockaddr_in& address) {
    SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    require(socketHandle != INVALID_SOCKET, "Could not create UDP socket");
    address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(socketHandle, reinterpret_cast<sockaddr*>(&address),
                 sizeof(address)) != SOCKET_ERROR,
            "Could not bind UDP socket");
    int length = sizeof(address);
    require(getsockname(socketHandle, reinterpret_cast<sockaddr*>(&address),
                        &length) != SOCKET_ERROR,
            "Could not inspect UDP socket");
    return socketHandle;
}

std::vector<char> makeData(std::uint32_t sequence,
                           const std::string& payload) {
    RdtHeader header{};
    header.seq_num = sequence;
    header.flags = FLAG_DATA;
    header.window_size = 1;
    header.payload_len = static_cast<std::uint16_t>(payload.size());
    std::vector<char> bytes(HEADER_SIZE + payload.size());
    serializeHeader(header, bytes.data());
    std::copy(payload.begin(), payload.end(), bytes.begin() + HEADER_SIZE);
    header.checksum = internetChecksum(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    serializeHeader(header, bytes.data());
    return bytes;
}

std::vector<char> makeAck(std::uint32_t acknowledgement) {
    RdtHeader header{};
    header.ack_num = acknowledgement;
    header.flags = FLAG_ACK;
    header.window_size = static_cast<std::uint16_t>(RDT_RECEIVE_WINDOW);
    std::vector<char> bytes(HEADER_SIZE);
    serializeHeader(header, bytes.data());
    header.checksum = internetChecksum(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    serializeHeader(header, bytes.data());
    return bytes;
}

void testOutOfWindowPacketIsNotAcknowledged() {
    sockaddr_in receiverAddress{};
    SOCKET receiverSocket = bindUdp(receiverAddress);
    RdtReceiver receiver(receiverSocket);

    sockaddr_in senderAddress{};
    SOCKET sender = bindUdp(senderAddress);
    const DWORD timeout = 200;
    setsockopt(sender, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    auto receiveResult = std::async(std::launch::async, [&] {
        std::uint32_t sequence = 99;
        std::vector<char> payload;
        bool isFinal = false;
        return receiver.receiveNext(sequence, payload, isFinal) &&
               sequence == 0 &&
               std::string(payload.begin(), payload.end()) == "accepted";
    });

    const auto outside = makeData(
        static_cast<std::uint32_t>(RDT_RECEIVE_WINDOW), "outside");
    require(sendto(sender, outside.data(), static_cast<int>(outside.size()), 0,
                   reinterpret_cast<sockaddr*>(&receiverAddress),
                   sizeof(receiverAddress)) != SOCKET_ERROR,
            "Could not send out-of-window packet");

    char response[HEADER_SIZE + MAX_PAYLOAD];
    sockaddr_in from{};
    int fromLength = sizeof(from);
    require(recvfrom(sender, response, sizeof(response), 0,
                     reinterpret_cast<sockaddr*>(&from), &fromLength) ==
                SOCKET_ERROR && WSAGetLastError() == WSAETIMEDOUT,
            "Receiver ACKed a packet outside its receive window");

    const auto accepted = makeData(0, "accepted");
    require(sendto(sender, accepted.data(), static_cast<int>(accepted.size()), 0,
                   reinterpret_cast<sockaddr*>(&receiverAddress),
                   sizeof(receiverAddress)) != SOCKET_ERROR,
            "Could not send in-window packet");
    require(recvfrom(sender, response, sizeof(response), 0,
                     reinterpret_cast<sockaddr*>(&from), &fromLength) > 0,
            "Receiver did not ACK an accepted packet");
    require(receiveResult.get(), "Receiver did not deliver the accepted packet");

    closesocket(sender);
    std::cout << "[PASS] out-of-window packet is dropped without ACK\n";
}

void testAckFromWrongPortIsIgnored() {
    sockaddr_in receiverAddress{};
    SOCKET receiver = bindUdp(receiverAddress);
    sockaddr_in attackerAddress{};
    SOCKET attacker = bindUdp(attackerAddress);

    RdtSender sender("127.0.0.1", ntohs(receiverAddress.sin_port), 300);
    require(sender.isValid(), "Could not construct RDT sender");
    auto sendResult = std::async(std::launch::async, [&] {
        return sender.sendChunk(0, "payload", 7, true) && sender.flush();
    });

    char data[HEADER_SIZE + MAX_PAYLOAD];
    sockaddr_in senderAddress{};
    int senderLength = sizeof(senderAddress);
    require(recvfrom(receiver, data, sizeof(data), 0,
                     reinterpret_cast<sockaddr*>(&senderAddress),
                     &senderLength) > 0,
            "Did not receive sender DATA packet");

    const auto acknowledgement = makeAck(0);
    require(sendto(attacker, acknowledgement.data(),
                   static_cast<int>(acknowledgement.size()), 0,
                   reinterpret_cast<sockaddr*>(&senderAddress),
                   sizeof(senderAddress)) != SOCKET_ERROR,
            "Could not send wrong-port ACK");
    require(sendResult.wait_for(std::chrono::milliseconds(100)) ==
                std::future_status::timeout,
            "Sender accepted an ACK from the wrong UDP port");

    require(sendto(receiver, acknowledgement.data(),
                   static_cast<int>(acknowledgement.size()), 0,
                   reinterpret_cast<sockaddr*>(&senderAddress),
                   sizeof(senderAddress)) != SOCKET_ERROR,
            "Could not send valid ACK");
    require(sendResult.get(), "Sender rejected ACK from the pinned endpoint");

    closesocket(receiver);
    closesocket(attacker);
    std::cout << "[PASS] ACK endpoint is pinned by IP and port\n";
}

void testInvalidExistingSocketsAreSafe() {
    RdtReceiver receiver(INVALID_SOCKET);
    RdtSender sender(INVALID_SOCKET);
    require(!receiver.isValid() && !sender.isValid(),
            "Invalid existing sockets were reported as valid");
    std::cout << "[PASS] invalid socket construction is safely destructible\n";
}

}

int main() {
    try {
        WinsockSession winsock;
        testOutOfWindowPacketIsNotAcknowledged();
        testAckFromWrongPortIsIgnored();
        testInvalidExistingSocketsAreSafe();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
