#pragma once

#include <fstream>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

class ChunkedFileReader {
public:
    static constexpr size_t CHUNK_SIZE = 1024;  // Re-check with Hung - member B

    // @param source path
    explicit ChunkedFileReader(const fs::path& filePath);
    bool isOpen() const;
    uintmax_t getTotalSize() const;

    bool nextChunk(std::vector<char>& chunk);

private:
    std::ifstream stream;
    uintmax_t totalSize;
};