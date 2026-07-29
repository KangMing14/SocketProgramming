#include "CheckSum.h"

uint16_t internetChecksum(const uint8_t *data, size_t len)
{
    uint32_t sum = 0;

    for (size_t i = 0; i + 1 < len; i += 2)
    {
        sum += (data[i] << 8) | data[i + 1];
    }

    if (len % 2 != 0)
    {
        sum += (data[len - 1] << 8);
    }

    while (sum >> 16)
    {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return static_cast<uint16_t>(~sum);
}