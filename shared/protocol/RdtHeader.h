#pragma once

#include "ProtocolConstants.h"

#include <cstddef>
#include <cstdint>
#include <vector>

enum RdtFlags : std::uint8_t {
    FLAG_SYN = 1 << 0,
    FLAG_ACK = 1 << 1,
    FLAG_FIN = 1 << 2,
    FLAG_DATA = 1 << 3,
    FLAG_NAK = 1 << 4
};

class IRdtTransport {
public:
    virtual ~IRdtTransport() = default;
    virtual bool sendChunk(std::uint32_t seqNum, const char* data,
                           std::size_t len, bool isFinal = false) = 0;
    virtual bool receiveNext(std::uint32_t& outSeqNum,
                             std::vector<char>& outData,
                             bool& outIsFinal) = 0;
    virtual bool flush() { return true; }
};

#pragma pack(push, 1)
struct RdtHeader {
    std::uint32_t seq_num;
    std::uint32_t ack_num;
    std::uint8_t flags;
    std::uint16_t window_size;
    std::uint16_t payload_len;
    std::uint16_t checksum;
    std::uint8_t reserved;
};

struct RdtPacket {
    RdtHeader header;
    char payload[MAX_PAYLOAD];
};
#pragma pack(pop)

void serializeHeader(const RdtHeader& header, char* buffer);
RdtHeader deserializeHeader(const char* buffer);
bool decodeValidatedDatagram(const char* bytes, std::size_t length,
                             RdtHeader& header);

constexpr std::size_t HEADER_SIZE = sizeof(RdtHeader);
static_assert(HEADER_SIZE == 16, "header must be 16 bytes on the wire");
