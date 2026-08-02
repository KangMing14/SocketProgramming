#pragma once

#include <cstddef>

constexpr std::size_t MAX_PAYLOAD = 1024;
constexpr int HANDSHAKE_TIMEOUT_MS = 500;
constexpr int HANDSHAKE_MAX_RETRIES = 10;
constexpr int DATA_IDLE_TIMEOUT_MS = 5000;
constexpr int CHAOS_DATA_IDLE_TIMEOUT_MS = 30000;
