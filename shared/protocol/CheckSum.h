#pragma once

#include <cstddef>
#include <cstdint>

std::uint16_t internetChecksum(const std::uint8_t* data, std::size_t len);
