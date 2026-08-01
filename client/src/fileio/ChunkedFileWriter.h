#pragma once

#include <fstream>
#include <map>
#include <filesystem>
#include <cstdint>
#include <vector>
#include <functional>

namespace fs = std::filesystem;

namespace hybridftp::client {

class ChunkedFileWriter {
public:

    // @param destination path
    explicit ChunkedFileWriter(const fs::path& filePath);
    bool isValid() const;

    void addChunk(uint32_t seqNum, const std::vector<char>& data);
    bool finalize(uint32_t expectedChunkCount, 
                    const std::function<std::vector<char>(const std::vector<char>&)>& transform = nullptr);

private:
    fs::path filePath;
    std::map<uint32_t, std::vector<char>> pendingChunks;
};

}
