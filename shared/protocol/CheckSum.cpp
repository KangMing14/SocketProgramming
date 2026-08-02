#include "CheckSum.h"

std::uint16_t internetChecksum(const std::uint8_t* data, std::size_t len) {
    std::uint32_t sum = 0;
    for (std::size_t i = 0; i + 1 < len; i += 2) {
        sum += (data[i] << 8) | data[i + 1];
    }
    if (len % 2 != 0) {
        sum += data[len - 1] << 8;
    }
    while (sum >> 16) {
        sum = (sum & 0xffffU) + (sum >> 16);
    }
    return static_cast<std::uint16_t>(~sum);
}
