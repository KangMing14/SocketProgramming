#pragma once

#include "RdtHeader.h"
#include "TransferMode.h"

#include <filesystem>

namespace hybridftp::client {

class DataChannelSession {
public:
    explicit DataChannelSession(IRdtTransport& transport);

    bool sendFile(const std::filesystem::path& filePath,
                  TransferMode mode = TransferMode::Binary);
    bool receiveFile(const std::filesystem::path& destination,
                     TransferMode mode = TransferMode::Binary);

private:
    IRdtTransport& transport;
};

}
