#pragma once

#include "RdtHeader.h"
#include "TransferMode.h"

#include <filesystem>
#include <functional>
#include <string>

namespace hybridftp::client {

struct SendResult {
    bool success = false;
    std::string sha256;

    operator bool() const noexcept { return success; }
};

class DataChannelSession {
public:
    using AbortPredicate = std::function<bool()>;

    explicit DataChannelSession(IRdtTransport& transport,
                                AbortPredicate abortRequested = {});

    SendResult sendFile(const std::filesystem::path& filePath,
                        TransferMode mode = TransferMode::Binary);
    bool receiveFile(const std::filesystem::path& destination,
                     TransferMode mode = TransferMode::Binary);

private:
    IRdtTransport& transport;
    AbortPredicate abortRequested;

    bool isAborted() const;
};

}
