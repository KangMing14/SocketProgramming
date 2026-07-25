#pragma once
#include <cstdint>

#define MAX_PAYLOAD 1000

enum RdtFlags : uint8_t
{
    FLAG_SYN = 1 << 0,
    FLAG_ACK = 1 << 1,
    FLAG_FIN = 1 << 2,
    FLAG_DATA = 1 << 3,
    FLAG_NAK = 1 << 4
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