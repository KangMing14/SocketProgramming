#include "RdtHeader.h"

#include <winsock2.h>
#include <cstring>

void serializeHeader(const RdtHeader& header, char* buffer) {
    const std::uint32_t seq = htonl(header.seq_num);
    const std::uint32_t ack = htonl(header.ack_num);
    const std::uint16_t window = htons(header.window_size);
    const std::uint16_t payloadLength = htons(header.payload_len);
    const std::uint16_t checksum = htons(header.checksum);

    std::memcpy(buffer, &seq, 4);
    std::memcpy(buffer + 4, &ack, 4);
    buffer[8] = header.flags;
    std::memcpy(buffer + 9, &window, 2);
    std::memcpy(buffer + 11, &payloadLength, 2);
    std::memcpy(buffer + 13, &checksum, 2);
    buffer[15] = header.reserved;
}

RdtHeader deserializeHeader(const char* buffer) {
    RdtHeader header{};
    std::uint32_t seq = 0;
    std::uint32_t ack = 0;
    std::uint16_t window = 0;
    std::uint16_t payloadLength = 0;
    std::uint16_t checksum = 0;

    std::memcpy(&seq, buffer, 4);
    std::memcpy(&ack, buffer + 4, 4);
    header.flags = static_cast<std::uint8_t>(buffer[8]);
    std::memcpy(&window, buffer + 9, 2);
    std::memcpy(&payloadLength, buffer + 11, 2);
    std::memcpy(&checksum, buffer + 13, 2);
    header.reserved = static_cast<std::uint8_t>(buffer[15]);

    header.seq_num = ntohl(seq);
    header.ack_num = ntohl(ack);
    header.window_size = ntohs(window);
    header.payload_len = ntohs(payloadLength);
    header.checksum = ntohs(checksum);
    return header;
}
