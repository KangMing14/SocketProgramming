#pragma once

#include <filesystem>
#include <string>
#include <vector>
#include <cstdint>
#include <functional>
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
    using AbortPredicate = std::function<bool()>;

    explicit DataChannelSession(IRdtTransport& transport,
                                AbortPredicate abortRequested = {});

    SendResult sendFile(const fs::path& filePath,
                        TransferMode mode = TransferMode::Binary);
    bool receiveFile(const fs::path& destPath, TransferMode mode = TransferMode::Binary);

private:
    IRdtTransport& transport;
    AbortPredicate abortRequested;

    bool isAborted() const;
};
