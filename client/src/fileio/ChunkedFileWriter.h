#pragma once

#include <fstream>
#include <filesystem>
#include <cstdint>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace hybridftp::client {

class ChunkedFileWriter {
public:
    explicit ChunkedFileWriter(const fs::path& filePath);
    ~ChunkedFileWriter();

    ChunkedFileWriter(const ChunkedFileWriter&) = delete;
    ChunkedFileWriter& operator=(const ChunkedFileWriter&) = delete;

    bool isValid() const;
    static bool validateDestination(const fs::path& filePath,
                                    std::string& failureReason);
    bool appendChunk(uint32_t seqNum, const std::vector<char>& data);
    bool commit();
    void abort();
    const std::string& errorMessage() const { return failureReason; }

private:
    fs::path destinationPath;
    fs::path temporaryPath;
    std::ofstream output;
    uint32_t nextExpectedSequence = 0;
    bool committed = false;
    bool valid = false;
    std::string failureReason;
};

}
