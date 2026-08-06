#pragma once

#include "RdtHeader.h"
#include "TransferMode.h"

#include <filesystem>
#include <string>

namespace hybridftp::client {

struct SendResult {
    bool success = false;
    std::string sha256;

    operator bool() const noexcept { return success; }
};

class DataChannelSession {
public:
    explicit DataChannelSession(IRdtTransport& transport);

    SendResult sendFile(const std::filesystem::path& filePath,
                        TransferMode mode = TransferMode::Binary);
    bool receiveFile(const std::filesystem::path& destination,
                     TransferMode mode = TransferMode::Binary);

private:
    IRdtTransport& transport;
};

}
