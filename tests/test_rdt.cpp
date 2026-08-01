#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

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

    require(
        internetChecksum(
            reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size()) == 0,
        "A valid serialized header should have a zero verification checksum");

    buffer[2] ^= 0x01;
    require(
        internetChecksum(
            reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size()) != 0,
        "Checksum failed to detect a flipped bit");

    std::cout << "[PASS] header serialization and checksum\n";
}

}  // namespace


int main() {
    try {
        WinsockSession winsock;
        testHeaderRoundTripAndChecksum();
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "[FAIL] " << exception.what() << '\n';
        return 1;
    }
}
