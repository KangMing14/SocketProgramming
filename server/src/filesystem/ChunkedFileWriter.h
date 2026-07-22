#pragma once

#include <fstream>
#include <map>
#include <filesystem>
#include <cstdint>
#include <vector>

namespace fs = std::filesystem;

class ChunkedFileWriter {
public:

    // @param destination path
    explicit ChunkedFileWriter(const fs::path& filePath);
    bool isValid() const;

    void addChunk(uint32_t seqNum, const std::vector<char>& data);
    bool finalize(uint32_t expectedChunkCount);

private:
    fs::path filePath;
    std::map<uint32_t, std::vector<char>> pendingChunks;
};