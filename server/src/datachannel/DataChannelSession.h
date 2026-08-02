#pragma once

#include <filesystem>
#include <vector>
#include <cstdint>
#include <winsock2.h>
#include "../rdt/RdtHeader.h"
#include "../common/TransferMode.h"

namespace fs = std::filesystem;

class DataChannelSession {
public:
    DataChannelSession(IRdtTransport& transport);

    bool sendFile(const fs::path& filePath, TransferMode mode = TransferMode::Binary);
    bool receiveFile(const fs::path& destPath, TransferMode mode = TransferMode::Binary);

private:
    IRdtTransport& transport;
};