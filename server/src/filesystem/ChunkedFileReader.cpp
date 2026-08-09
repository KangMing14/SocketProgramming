#include "ChunkedFileReader.h"

ChunkedFileReader::ChunkedFileReader(const fs::path& filePath)
    : stream(filePath, std::ios::binary), totalSize(0) {
    std::error_code ec;
    totalSize = fs::file_size(filePath, ec);
}

bool ChunkedFileReader::isOpen() const { return stream.is_open(); }

uintmax_t ChunkedFileReader::getTotalSize() const { return totalSize; }

bool ChunkedFileReader::nextChunk(std::vector<char>& chunk) {
    chunk.resize(CHUNK_SIZE);
    stream.read(chunk.data(), CHUNK_SIZE);
    size_t bytesRead = static_cast<size_t>(stream.gcount());
    chunk.resize(bytesRead);
    return bytesRead > 0;
}