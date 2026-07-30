#pragma once
#include <cstdint>
#include <vector>

#include "../common/ProtocolConstants.h"

enum RdtFlags : uint8_t
{
    FLAG_SYN = 1 << 0,
    FLAG_ACK = 1 << 1,
    FLAG_FIN = 1 << 2,
    FLAG_DATA = 1 << 3,
    FLAG_NAK = 1 << 4
};

class IRdtTransport {
public:
    virtual ~IRdtTransport() = default;
    virtual bool sendChunk(uint32_t seqNum, const char* data, size_t len) = 0;
    virtual bool receiveNext(uint32_t& outSeqNum, std::vector<char>& outData, bool& outIsFinal) = 0;
    virtual bool waitForClientReady() { return true; }
};

#pragma pack(push, 1)

struct RdtHeader
{
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t flags;
    uint16_t window_size;
    uint16_t payload_len;
    uint16_t checksum;
    uint8_t reserved;
};

struct RdtPacket
{
    RdtHeader header;
    char payload[MAX_PAYLOAD];
};

#pragma pack(pop)

void serializeHeader(const RdtHeader &h, char *buf);
RdtHeader deserializeHeader(const char *buf);

static_assert(sizeof(RdtHeader) == 16, "header must be 16 bytes on the wire");