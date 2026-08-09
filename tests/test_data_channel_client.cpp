#include "../client/src/datachannel/ActiveModeClient.h"
#include "../client/src/datachannel/DataChannelSession.h"
#include "../client/src/rdt/RdtReceiver.h"
#include "../client/src/rdt/RdtSender.h"
#include "CheckSum.h"
#include "helper.h"

#include <winsock2.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <future>
#include <iostream>

namespace {

SOCKET bindLoopbackUdp(sockaddr_in& address) {
    SOCKET socketHandle = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(socketHandle != INVALID_SOCKET);
    address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    assert(bind(socketHandle, reinterpret_cast<sockaddr*>(&address),
                sizeof(address)) != SOCKET_ERROR);
    int length = sizeof(address);
    assert(getsockname(socketHandle, reinterpret_cast<sockaddr*>(&address),
                       &length) != SOCKET_ERROR);
    return socketHandle;
}

std::vector<char> makeAcknowledgement(std::uint32_t acknowledgementNumber) {
    RdtHeader header{};
    header.ack_num = acknowledgementNumber;
    header.flags = FLAG_ACK;
    header.window_size = 1;
    std::vector<char> bytes(HEADER_SIZE);
    serializeHeader(header, bytes.data());
    header.checksum = internetChecksum(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    serializeHeader(header, bytes.data());
    return bytes;
}

void testBinaryRoundTrip() {
    createTestFile("dc_client_source.bin", 2600);
    FakeRdtTransport transport;
    hybridftp::client::DataChannelSession sender(transport);

    const auto result = sender.sendFile("dc_client_source.bin");
    assert(result.success);
    assert(result.sha256.size() == 64);
    assert(!transport.sentChunks.empty());
    assert(transport.sentFinalFlags.back());

    hybridftp::client::DataChannelSession receiver(transport);
    assert(receiver.receiveFile("dc_client_result.bin"));
    assert(filesAreIdentical("dc_client_source.bin", "dc_client_result.bin"));
    std::cout << "[PASS] client binary data-channel round trip\n";
}

void testEmptyFileCarriesFinalPacket() {
    std::ofstream("dc_client_empty.bin", std::ios::binary).close();
    FakeRdtTransport transport;
    hybridftp::client::DataChannelSession sender(transport);

    const auto result = sender.sendFile("dc_client_empty.bin");
    assert(result.success);
    assert(transport.sentChunks.size() == 1);
    assert(transport.sentChunks.front().second.empty());
    assert(transport.sentFinalFlags.size() == 1);
    assert(transport.sentFinalFlags.front());

    hybridftp::client::DataChannelSession receiver(transport);
    assert(receiver.receiveFile("dc_client_empty_result.bin"));
    assert(filesAreIdentical(
        "dc_client_empty.bin", "dc_client_empty_result.bin"));
    std::cout << "[PASS] client empty file uses DATA|FIN\n";
}

void testAbortBeforeSend() {
    createTestFile("dc_client_abort.bin", 100);
    FakeRdtTransport transport;
    hybridftp::client::DataChannelSession session(
        transport, [] { return true; });

    const auto result = session.sendFile("dc_client_abort.bin");
    assert(!result.success);
    assert(transport.sentChunks.empty());
    std::cout << "[PASS] client abort prevents data transmission\n";
}

void testEarlyDataCompletesPassiveHandshake() {
    SOCKET server = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(server != INVALID_SOCKET);
    sockaddr_in serverAddress{};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    serverAddress.sin_port = 0;
    assert(bind(server, reinterpret_cast<sockaddr*>(&serverAddress),
                sizeof(serverAddress)) != SOCKET_ERROR);
    int serverLength = sizeof(serverAddress);
    assert(getsockname(server, reinterpret_cast<sockaddr*>(&serverAddress),
                       &serverLength) != SOCKET_ERROR);

    SOCKET clientSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    assert(clientSocket != INVALID_SOCKET);
    sockaddr_in clientAddress{};
    clientAddress.sin_family = AF_INET;
    clientAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    clientAddress.sin_port = 0;
    assert(bind(clientSocket, reinterpret_cast<sockaddr*>(&clientAddress),
                sizeof(clientAddress)) != SOCKET_ERROR);

    hybridftp::client::RdtReceiver receiver(clientSocket);
    auto ready = std::async(std::launch::async, [&] {
        return receiver.signalClientReady(serverAddress);
    });

    char synBytes[HEADER_SIZE + MAX_PAYLOAD]{};
    sockaddr_in from{};
    int fromLength = sizeof(from);
    const int synLength = recvfrom(
        server, synBytes, sizeof(synBytes), 0,
        reinterpret_cast<sockaddr*>(&from), &fromLength);
    RdtHeader syn{};
    assert(synLength > 0 && decodeValidatedDatagram(
        synBytes, static_cast<std::size_t>(synLength), syn));
    assert((syn.flags & FLAG_SYN) != 0);

    const std::string payload = "early";
    RdtHeader data{};
    data.seq_num = 0;
    data.flags = static_cast<std::uint8_t>(FLAG_DATA | FLAG_FIN);
    data.window_size = 1;
    data.payload_len = static_cast<std::uint16_t>(payload.size());
    std::vector<char> dataBytes(HEADER_SIZE + payload.size());
    serializeHeader(data, dataBytes.data());
    std::copy(payload.begin(), payload.end(), dataBytes.begin() + HEADER_SIZE);
    data.checksum = internetChecksum(
        reinterpret_cast<const std::uint8_t*>(dataBytes.data()),
        dataBytes.size());
    serializeHeader(data, dataBytes.data());
    assert(sendto(server, dataBytes.data(), static_cast<int>(dataBytes.size()),
                  0, reinterpret_cast<sockaddr*>(&from), sizeof(from)) !=
           SOCKET_ERROR);

    assert(ready.get());
    std::uint32_t sequence = 99;
    std::vector<char> received;
    bool isFinal = false;
    assert(receiver.receiveNext(sequence, received, isFinal));
    assert(sequence == 0 && isFinal);
    assert(std::string(received.begin(), received.end()) == payload);

    fd_set readSet;
    FD_ZERO(&readSet);
    FD_SET(server, &readSet);
    timeval waitBeforeCommit{};
    waitBeforeCommit.tv_usec = 100000;
    assert(select(0, &readSet, nullptr, nullptr, &waitBeforeCommit) == 0);

    assert(receiver.confirmReceive(sequence, true));
    char ackBytes[HEADER_SIZE]{};
    sockaddr_in ackFrom{};
    int ackFromLength = sizeof(ackFrom);
    const int ackLength = recvfrom(
        server, ackBytes, sizeof(ackBytes), 0,
        reinterpret_cast<sockaddr*>(&ackFrom), &ackFromLength);
    RdtHeader acknowledgement{};
    assert(ackLength == static_cast<int>(HEADER_SIZE) &&
           decodeValidatedDatagram(
               ackBytes, static_cast<std::size_t>(ackLength), acknowledgement));
    assert((acknowledgement.flags & FLAG_ACK) != 0 &&
           acknowledgement.ack_num == sequence);

    closesocket(server);
    std::cout << "[PASS] final DATA is acknowledged after client commit\n";
}

void testClientSenderRejectsWrongAckEndpoint() {
    sockaddr_in receiverAddress{};
    SOCKET receiver = bindLoopbackUdp(receiverAddress);
    sockaddr_in attackerAddress{};
    SOCKET attacker = bindLoopbackUdp(attackerAddress);

    hybridftp::client::RdtSender sender(
        "127.0.0.1", ntohs(receiverAddress.sin_port), 300);
    assert(sender.isValid());
    auto result = std::async(std::launch::async, [&] {
        return sender.sendChunk(0, "client", 6, true);
    });

    char data[HEADER_SIZE + MAX_PAYLOAD];
    sockaddr_in senderAddress{};
    int senderLength = sizeof(senderAddress);
    assert(recvfrom(receiver, data, sizeof(data), 0,
                    reinterpret_cast<sockaddr*>(&senderAddress),
                    &senderLength) > 0);

    const auto ack = makeAcknowledgement(0);
    assert(sendto(attacker, ack.data(), static_cast<int>(ack.size()), 0,
                  reinterpret_cast<sockaddr*>(&senderAddress),
                  sizeof(senderAddress)) != SOCKET_ERROR);
    assert(result.wait_for(std::chrono::milliseconds(100)) ==
           std::future_status::timeout);

    assert(sendto(receiver, ack.data(), static_cast<int>(ack.size()), 0,
                  reinterpret_cast<sockaddr*>(&senderAddress),
                  sizeof(senderAddress)) != SOCKET_ERROR);
    assert(result.get());

    closesocket(receiver);
    closesocket(attacker);
    std::cout << "[PASS] client sender rejects ACK from wrong UDP port\n";
}

void testManualPortArgumentParsingAndBinding() {
    SOCKET temporarySocket = INVALID_SOCKET;
    unsigned short availablePort = 0;
    assert(hybridftp::client::openActiveListenPort(
        temporarySocket, availablePort));
    closesocket(temporarySocket);

    in_addr loopback{};
    assert(inet_pton(AF_INET, "127.0.0.1", &loopback) == 1);
    const std::string argument = hybridftp::client::formatPortCommand(
        ntohl(loopback.s_addr), availablePort);

    sockaddr_in requestedAddress{};
    assert(hybridftp::client::parsePortCommand(
        argument, requestedAddress));
    assert(requestedAddress.sin_addr.s_addr == loopback.s_addr);
    assert(ntohs(requestedAddress.sin_port) == availablePort);

    SOCKET manuallyBoundSocket = INVALID_SOCKET;
    assert(hybridftp::client::openActiveListenPort(
        requestedAddress, manuallyBoundSocket));
    sockaddr_in boundAddress{};
    int boundLength = sizeof(boundAddress);
    assert(getsockname(
        manuallyBoundSocket, reinterpret_cast<sockaddr*>(&boundAddress),
        &boundLength) != SOCKET_ERROR);
    assert(boundAddress.sin_addr.s_addr == loopback.s_addr);
    assert(ntohs(boundAddress.sin_port) == availablePort);
    closesocket(manuallyBoundSocket);

    for (const std::string invalid : {
             "127,0,0,1,1", "127,0,0,1,1,2,3",
             "127,0,0,1,0,0", "127,0,0,999,1,2",
             "127,0,0,1,1,2junk"}) {
        sockaddr_in rejected{};
        assert(!hybridftp::client::parsePortCommand(invalid, rejected));
    }
    std::cout << "[PASS] manual PORT argument binds the requested endpoint\n";
}

}

int main() {
    WSADATA data{};
    assert(WSAStartup(MAKEWORD(2, 2), &data) == 0);
    testBinaryRoundTrip();
    testEmptyFileCarriesFinalPacket();
    testAbortBeforeSend();
    testEarlyDataCompletesPassiveHandshake();
    testClientSenderRejectsWrongAckEndpoint();
    testManualPortArgumentParsingAndBinding();

    for (const auto& name : {
             "dc_client_source.bin", "dc_client_result.bin",
             "dc_client_empty.bin", "dc_client_empty_result.bin",
             "dc_client_abort.bin"}) {
        std::error_code error;
        fs::remove(name, error);
    }
    WSACleanup();
    std::cout << "\n---All client data-channel tests passed---\n";
}
