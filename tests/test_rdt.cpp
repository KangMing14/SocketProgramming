#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <winsock2.h>

#include "CheckSum.h"
#include "RdtHeader.h"

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class WinsockSession {
public:
    WinsockSession() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            throw std::runtime_error(
                "WSAStartup failed with error " + std::to_string(result));
        }
    }

    ~WinsockSession() {
        WSACleanup();
    }
};

void testHeaderRoundTripAndChecksum() {
    RdtHeader original{};
    original.seq_num = 42;
    original.ack_num = 100;
    original.flags = FLAG_DATA;
    original.window_size = 8;
    original.payload_len = 500;

    std::array<char, HEADER_SIZE> buffer{};
    serializeHeader(original, buffer.data());
    original.checksum = internetChecksum(
        reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size());
    serializeHeader(original, buffer.data());

    const RdtHeader roundTrip = deserializeHeader(buffer.data());

    require(roundTrip.seq_num == original.seq_num,
            "Sequence number changed after serialization round trip");
    require(roundTrip.ack_num == original.ack_num,
            "ACK number changed after serialization round trip");
    require(roundTrip.flags == original.flags,
            "Flags changed after serialization round trip");
    require(roundTrip.window_size == original.window_size,
            "Window size changed after serialization round trip");
    require(roundTrip.payload_len == original.payload_len,
            "Payload length changed after serialization round trip");
    require(roundTrip.checksum == original.checksum,
            "Checksum changed after serialization round trip");

    // Verify the checksum using the same contract as the production receiver:
    // extract the transmitted value, clear the checksum field, then recompute.
    // This avoids assuming that internetChecksum() can verify an embedded
    // checksum by returning zero, which depends on its byte-order convention.
    auto verificationBuffer = buffer;
    verificationBuffer[13] = 0;
    verificationBuffer[14] = 0;

    const uint16_t recomputedChecksum = internetChecksum(
        reinterpret_cast<const uint8_t*>(verificationBuffer.data()),
        verificationBuffer.size());

    require(recomputedChecksum == roundTrip.checksum,
            "A valid serialized header should preserve its checksum");

    buffer[2] ^= 0x01;
    verificationBuffer = buffer;
    verificationBuffer[13] = 0;
    verificationBuffer[14] = 0;

    require(
        internetChecksum(
            reinterpret_cast<const uint8_t*>(verificationBuffer.data()),
            verificationBuffer.size()) != roundTrip.checksum,
        "Checksum failed to detect a flipped bit");

    std::cout << "[PASS] header serialization and checksum\n";
}

std::vector<char> makeDatagram(std::uint8_t flags,
                               std::uint16_t declaredLength,
                               std::size_t actualLength) {
    RdtHeader header{};
    header.seq_num = 7;
    header.flags = flags;
    header.window_size = 1;
    header.payload_len = declaredLength;

    std::vector<char> bytes(HEADER_SIZE + actualLength, 'x');
    serializeHeader(header, bytes.data());
    header.checksum = internetChecksum(
        reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
    serializeHeader(header, bytes.data());
    return bytes;
}

void testStrictDatagramValidation() {
    RdtHeader decoded{};

    const auto valid = makeDatagram(FLAG_DATA, 3, 3);
    require(decodeValidatedDatagram(valid.data(), valid.size(), decoded),
            "Valid DATA datagram was rejected");
    require(decoded.payload_len == 3, "Validated payload length changed");

    const auto truncated = makeDatagram(FLAG_DATA, 3, 2);
    require(!decodeValidatedDatagram(
                truncated.data(), truncated.size(), decoded),
            "Truncated payload was accepted");

    const auto trailing = makeDatagram(FLAG_DATA, 2, 3);
    require(!decodeValidatedDatagram(trailing.data(), trailing.size(), decoded),
            "Trailing payload bytes were accepted");

    const auto oversized = makeDatagram(
        FLAG_DATA, static_cast<std::uint16_t>(MAX_PAYLOAD + 1), 0);
    require(!decodeValidatedDatagram(
                oversized.data(), oversized.size(), decoded),
            "Oversized declared payload was accepted");

    const auto ackWithPayload = makeDatagram(FLAG_ACK, 1, 1);
    require(!decodeValidatedDatagram(
                ackWithPayload.data(), ackWithPayload.size(), decoded),
            "ACK with payload was accepted");

    const auto emptyFinal = makeDatagram(
        static_cast<std::uint8_t>(FLAG_DATA | FLAG_FIN), 0, 0);
    require(decodeValidatedDatagram(
                emptyFinal.data(), emptyFinal.size(), decoded),
            "Empty DATA|FIN datagram was rejected");

    const auto maximum = makeDatagram(
        FLAG_DATA, static_cast<std::uint16_t>(MAX_PAYLOAD), MAX_PAYLOAD);
    require(decodeValidatedDatagram(maximum.data(), maximum.size(), decoded),
            "MAX_PAYLOAD datagram was rejected");
    std::cout << "[PASS] strict datagram length and control validation\n";
}

}  // namespace


int main() {
    try {
        WinsockSession winsock;
        testHeaderRoundTripAndChecksum();
        testStrictDatagramValidation();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
