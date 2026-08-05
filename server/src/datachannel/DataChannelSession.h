#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
#include <winsock2.h>
#include "../rdt/RdtHeader.h"
#include "../common/TransferMode.h"

namespace fs = std::filesystem;

struct SendResult {
    bool success = false;
    std::string sha256;

    operator bool() const noexcept { return success; }
};

class DataChannelSession {
public:
    DataChannelSession(IRdtTransport& transport);

    SendResult sendFile(const fs::path& filePath,
                        TransferMode mode = TransferMode::Binary);
    bool receiveFile(const fs::path& destPath, TransferMode mode = TransferMode::Binary);

private:
    IRdtTransport& transport;
};
