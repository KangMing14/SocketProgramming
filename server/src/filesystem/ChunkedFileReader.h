#pragma once

#include <fstream>
#include <vector>
#include <filesystem>

#include "ProtocolConstants.h"

namespace fs = std::filesystem;

class ChunkedFileReader {
public:
    static constexpr size_t CHUNK_SIZE = MAX_PAYLOAD;

    // @param source path
    explicit ChunkedFileReader(const fs::path& filePath);
    bool isOpen() const;
    uintmax_t getTotalSize() const;

    bool nextChunk(std::vector<char>& chunk);

private:
    std::ifstream stream;
    uintmax_t totalSize;
};